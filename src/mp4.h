#ifndef MELLAGUI_MP4_H
#define MELLAGUI_MP4_H

/*
 * Minimal MP4 (ISO base media file) writer for one video track and, optionally, one sound track (16 bit little endian PCM, "sowt": ffmpeg, mpv, VLC and
 * QuickTime play it, browsers do not; lm_mp4_set_audio / lm_mp4_add_audio): JPEG pictures (Motion JPEG: sample entry "mp4v" with an esds of
 * object type 0x6C, which ffmpeg, VLC, mpv and QuickTime play; browsers do not) or H.264 access units ("avc1" + avcC, which plays
 * everywhere; lm_mp4_set_h264 before the first picture). No audio, no fragments: the frames are
 * appended to a `mdat` box as they arrive and the `moov` box with the sample tables is written by lm_mp4_close(), so a recording
 * that was never closed (crash, power loss) is not playable. The file is created with mode 0600 (a screen shows secrets).
 * Frame times are in milliseconds (the track's time scale is 1000); a frame lasts until the next one. Owner: mp4.c.
 */

#include <stddef.h>
#include <stdint.h>

/* The functions are safe to call from several threads (the pictures come from encoder threads, the sound from the loop's thread): one lock inside. */

struct lm_mp4;

/* Creates `path` (it must not exist). NULL on failure (errno is set). */
struct lm_mp4 *lm_mp4_create(const char *path, int width, int height);
/* Makes it an H.264 track: the SPS and PPS NAL units (header byte included, as h264enc.c makes them). Before the first picture. 0 or -1. */
int lm_mp4_set_h264(struct lm_mp4 *m, const uint8_t *sps, size_t sps_len, const uint8_t *pps, size_t pps_len);
/* Appends one H.264 access unit (a 4 byte length before each NAL unit) shown from `pts_ms` on; `key` = an IDR picture (a place to start playing). */
int lm_mp4_add_sample(struct lm_mp4 *m, const uint8_t *data, size_t len, uint64_t pts_ms, int key);
/* Appends one JPEG picture shown from `pts_ms` on (milliseconds since any start; must not go backwards). 0 or -1 (disk full ...). */
int lm_mp4_add(struct lm_mp4 *m, const uint8_t *jpeg, size_t len, uint64_t pts_ms);
/* Adds a sound track: `channels` (1 or 2) channels of signed 16 bit little endian PCM at `rate` Hz. Before the first picture or the first sound. 0 or -1. */
int lm_mp4_set_audio(struct lm_mp4 *m, int rate, int channels);
/* Appends `frames` PCM frames (interleaved channels). `pts_ms` says when the FIRST frame of the whole sound was heard, on the same clock as the pictures'
 * times; it is read from the first call only (the frames after it follow each other without a gap). 0 or -1. */
int lm_mp4_add_audio(struct lm_mp4 *m, const int16_t *pcm, size_t frames, uint64_t pts_ms);
/* Number of pictures added so far / bytes written so far. */
uint32_t lm_mp4_frames(const struct lm_mp4 *m);
uint64_t lm_mp4_bytes(const struct lm_mp4 *m);
/* Writes the index; the last picture lasts until `end_ms`. Closes and frees `m` (also on failure, then the file is removed).
 * Returns 0, or -1. A file without any picture is removed too (-1). */
int lm_mp4_close(struct lm_mp4 *m, uint64_t end_ms);

#endif
