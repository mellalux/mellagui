#ifndef MELLAGUI_MP4READ_H
#define MELLAGUI_MP4READ_H

/*
 * MP4 / MOV reader for mellagui-view: the first video track's sample table (offsets, sizes, times) and the raw samples. Only Motion
 * JPEG can be shown (what mellagui-record writes, and what `ffmpeg -c:v mjpeg` makes): sample entry "mp4v" with an esds of object
 * type 0x6C, or "jpeg" / "MJPG", or H.264 ("avc1": the samples and the parameter sets are given, decoding is h264dec.c's business);
 * other codecs are opened (so the viewer can say what they are) but `mjpeg` and `h264` are false. No fragments, no
 * edit lists, no sound. The whole `moov` box is read into memory (at most 256 MB), the samples are read one at a time. Owner: mp4read.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

struct lm_mp4r {
    FILE *f;
    uint64_t file_size;
    int w, h;
    char codec[5];                  /* the sample entry's four characters, e.g. "mp4v", "avc1" */
    bool mjpeg;                     /* the samples are JPEG pictures */
    bool h264;                      /* the samples are H.264 access units (4 byte lengths); sps / pps are the parameter sets, NAL units with their header byte */
    uint8_t *sps, *pps;
    size_t sps_n, pps_n;
    uint8_t *key;                   /* n flags: the sample can be started at (all 1 for JPEG) */
    uint32_t n;                     /* samples (pictures) */
    uint64_t *off;                  /* n file offsets */
    uint32_t *size;                 /* n sizes */
    uint64_t *pts_ms;               /* n times in milliseconds, from 0 */
    uint64_t duration_ms;           /* end of the last picture */
};

/* NULL with a short message in err when the file is no usable MP4 (damaged, fragmented, no video track, tables beyond the file). */
struct lm_mp4r *lm_mp4r_open(const char *path, char *err, size_t errsz);
void lm_mp4r_close(struct lm_mp4r *r);
/* Sample i as a malloc'd buffer (*len bytes), NULL when out of range or unreadable. */
uint8_t *lm_mp4r_sample(struct lm_mp4r *r, uint32_t i, size_t *len);
/* The picture shown at `ms`: the last one whose time is <= ms (0 before the first). */
uint32_t lm_mp4r_frame_at(const struct lm_mp4r *r, uint64_t ms);

#endif
