#ifndef MELLAGUI_H264_H
#define MELLAGUI_H264_H

/*
 * H.264 pieces shared by mellagui's own encoder (h264enc.c) and decoder (h264dec.c): the CAVLC code tables (ITU-T H.264 tables 9-5, 9-7,
 * 9-8, 9-9, 9-10), the integer transforms, quantization scales and a bit writer / reader.
 *
 * What the two programs speak, and nothing else ("constrained baseline, the simplest useful subset"): 4:2:0, 8 bit, one slice per picture,
 * CAVLC, one reference picture, picture order count type 2 (no reordering), the deblocking filter OFF (disable_deblocking_filter_idc = 1),
 * macroblock types I_16x16 with DC prediction (luma and chroma), I_PCM, P_L0_16x16 with the motion vector (0, 0) and P_Skip. That is enough for
 * a screen recording (unchanged macroblocks cost a bit each) and small enough to be checked bit for bit against ffmpeg's decoder.
 */

#include <stddef.h>
#include <stdint.h>

/* ---- bit writer / reader ------------------------------------------------------------------------------------------------ */

struct lm_bw {
    uint8_t *p;
    size_t n, cap;                  /* bytes complete / allocated */
    uint64_t acc;                   /* bits waiting, the newest in the low bits */
    int nacc;
    int oom;
};

void lm_bw_put(struct lm_bw *b, uint32_t value, int nbits);      /* nbits 0..32 */
void lm_bw_ue(struct lm_bw *b, uint32_t v);
void lm_bw_se(struct lm_bw *b, int32_t v);
void lm_bw_trailing(struct lm_bw *b);                            /* the stop bit and zeros up to a byte boundary */
size_t lm_bw_bits(const struct lm_bw *b);
void lm_bw_free(struct lm_bw *b);

struct lm_br {
    const uint8_t *p;
    size_t nbits, pos;              /* total bits, position */
};

uint32_t lm_br_peek(const struct lm_br *r, int nbits);           /* up to 24 bits; zeros past the end */
uint32_t lm_br_get(struct lm_br *r, int nbits);                  /* up to 24 bits */
int lm_br_ue(struct lm_br *r, uint32_t *v);                      /* 0, or -1 on damaged data / the end */
int lm_br_se(struct lm_br *r, int32_t *v);

/* NAL unit: header byte + payload with emulation prevention (00 00 0x -> 00 00 03 0x). Returns malloc'd bytes. */
uint8_t *lm_h264_nal(int ref_idc, int type, const uint8_t *rbsp, size_t n, size_t *out_len);
/* The reverse: copies `n` bytes of NAL payload (after the header byte) without the 03 bytes into out (at most n bytes); returns the length. */
size_t lm_h264_unescape(const uint8_t *nal, size_t n, uint8_t *out);

/* ---- CAVLC tables --------------------------------------------------------------------------------------------------------- */

/* coeff_token: [table][4 * total_coeff + trailing_ones], table 0..3 for nC 0-1, 2-3, 4-7, 8+; chroma DC (nC = -1): [4 * total_coeff + trailing_ones]. */
extern const uint8_t lm_h264_coeff_token_len[4][68], lm_h264_coeff_token_bits[4][68];
extern const uint8_t lm_h264_cdc_token_len[20], lm_h264_cdc_token_bits[20];
extern const uint8_t lm_h264_total_zeros_len[15][16], lm_h264_total_zeros_bits[15][16];      /* [total_coeff - 1][total_zeros] */
extern const uint8_t lm_h264_cdc_zeros_len[3][4], lm_h264_cdc_zeros_bits[3][4];
extern const uint8_t lm_h264_run_len[7][16], lm_h264_run_bits[7][16];                        /* [min(zeros_left, 7) - 1][run_before] */
extern const uint8_t lm_h264_inter_cbp[48];                                                  /* code number -> coded_block_pattern (inter) */
extern const uint8_t lm_h264_zigzag4[16];                                                    /* scan position -> raster index of a 4x4 block */
extern const uint8_t lm_h264_blk_x[16], lm_h264_blk_y[16];                                   /* luma 4x4 block index -> position in 4x4 units */

int lm_h264_chroma_qp(int qp);

/* ---- transforms (8.5 of the standard) -------------------------------------------------------------------------------- */

/* Forward 4x4 integer transform of residuals r[16] (raster) into w[16]. */
void lm_h264_fdct4(const int *r, int *w);
/* Quantizes w[16] to levels[16] (raster) with the scales of `qp`; `intra` picks the dead zone. Returns the number of non-zero levels. */
int lm_h264_quant4(const int *w, int *levels, int qp, int intra, int skip_dc);
/* Dequantizes levels[16] and applies the inverse transform; the 16 residuals go to r (raster). If `dc` is not NULL it replaces the DC term
 * (already scaled: the DC of an Intra16x16 / chroma block comes from the Hadamard path). */
void lm_h264_dequant_idct4(const int *levels, int qp, const int *dc, int *r);
/* Luma DC (Intra16x16): the 16 DC terms of the forward transforms in raster order of the blocks -> levels (raster 4x4), and back. */
int lm_h264_quant_luma_dc(const int *dc16, int *levels, int qp);
void lm_h264_dequant_luma_dc(const int *levels, int qp, int *dc16);
/* Chroma DC: 4 terms -> levels, and back. */
int lm_h264_quant_chroma_dc(const int *dc4, int *levels, int qpc);
void lm_h264_dequant_chroma_dc(const int *levels, int qpc, int *dc4);

/* DC intra prediction values from the reconstructed neighbours (planes with their strides; mbx / mby = macroblock position): the Intra16x16 luma
 * one, and the one of the 4x4 chroma block (bx, by) (8.3.3 and 8.3.4 of the standard). */
int lm_h264_luma_dc_pred(const uint8_t *Y, int ys, int mbx, int mby);
int lm_h264_chroma_dc_pred(const uint8_t *plane, int cs, int mbx, int mby, int bx, int by);

static inline int lm_h264_clip(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

#endif
