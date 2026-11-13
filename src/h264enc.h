#ifndef MELLAGUI_H264ENC_H
#define MELLAGUI_H264ENC_H

/*
 * H.264 encoder for screen recordings, written from scratch (subset described in h264.h): RGB in, one access unit per picture out. The
 * first picture and every picture asked for are IDR pictures made of Intra16x16 DC macroblocks; the others are P pictures in which a
 * macroblock whose pixels are the same as in the previous picture costs one bit (P_Skip), one that changed a little carries a residual
 * against the previous reconstruction - moved by a whole-pixel motion vector when the content scrolled or slid (candidates: the neighbours', the
 * one of the picture before, the one a skip would use, a scroll of the whole picture found by matching row hashes; then small steps) -, one
 * that changed a lot is coded as intra. Constant quantizer: `qp` 0..51 (lower is better; 24 is a
 * good screen quality at 4:2:0). The colours are converted with BT.709 limited range and the stream says so (VUI).
 * Not thread safe: one encoder per thread. Owner: h264enc.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct lm_h264enc;

/* width and height are rounded down to even numbers (4:2:0); ask lm_h264enc_width() / _height() for what is coded. NULL on failure. */
struct lm_h264enc *lm_h264enc_create(int width, int height, int qp);
void lm_h264enc_free(struct lm_h264enc *e);
int lm_h264enc_width(const struct lm_h264enc *e);
int lm_h264enc_height(const struct lm_h264enc *e);

/* The sequence and picture parameter sets as NAL units (header byte included, no start code, no length), malloc'd. */
bool lm_h264enc_headers(const struct lm_h264enc *e, uint8_t **sps, size_t *sps_len, uint8_t **pps, size_t *pps_len);

/*
 * Encodes one picture (XRGB8888, `stride` in bytes) as one access unit in "AVCC" form (a 4 byte big endian length before each NAL unit, which is
 * what an MP4 sample holds). `idr` forces an IDR picture; *key tells whether it is one. malloc'd, NULL on failure.
 */
uint8_t *lm_h264enc_encode(struct lm_h264enc *e, const uint32_t *xrgb, size_t stride, bool idr, size_t *len, bool *key);

/* The reconstructed picture (what a decoder shows) as planes: 0 = Y, 1 = U, 2 = V; the stride is returned. For tests. */
const uint8_t *lm_h264enc_recon(const struct lm_h264enc *e, int plane, int *stride);

#endif
