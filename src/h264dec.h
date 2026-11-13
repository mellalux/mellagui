#ifndef MELLAGUI_H264DEC_H
#define MELLAGUI_H264DEC_H

/*
 * H.264 decoder for exactly the streams h264enc.c makes (the subset in h264.h): constrained baseline, one slice per picture, CAVLC,
 * Intra16x16 DC / I_PCM / P_L0_16x16 with whole-pixel motion vectors / P_Skip (its vector inferred), no deblocking filter. It lets mellagui-view play its own recordings
 * without ffmpeg. Anything else in a stream is refused with a message ("not supported: ..."), so the viewer can hand such a file to
 * ffmpeg instead. Not thread safe. Owner: h264dec.c.
 */

#include <stddef.h>
#include <stdint.h>

struct lm_h264dec;

/* From the SPS and PPS NAL units (header byte included). NULL, with a message in err, when the stream is something else. */
struct lm_h264dec *lm_h264dec_create(const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len, char *err, size_t errsz);
void lm_h264dec_free(struct lm_h264dec *d);
int lm_h264dec_width(const struct lm_h264dec *d);
int lm_h264dec_height(const struct lm_h264dec *d);

/* Decodes one access unit (4 byte big endian length before each NAL unit, as in an MP4 sample). A P picture needs the picture before it:
 * start at an IDR picture. 0 or -1 (message in err). */
int lm_h264dec_decode(struct lm_h264dec *d, const uint8_t *au, size_t len, char *err, size_t errsz);

/* The picture's planes (0 = Y, 1 = U, 2 = V) as decoded, with their strides: for tests. */
const uint8_t *lm_h264dec_plane(const struct lm_h264dec *d, int plane, int *stride);

/* The decoded picture as XRGB8888 (BT.709, limited range), width x height pixels, `stride_px` pixels a row. */
void lm_h264dec_xrgb(const struct lm_h264dec *d, uint32_t *out, size_t stride_px);

#endif
