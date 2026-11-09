#ifndef MELLAGUI_MP4_H
#define MELLAGUI_MP4_H

/*
 * Minimal MP4 (ISO base media file) writer for one video track of JPEG pictures (Motion JPEG: sample entry "mp4v" with an esds of
 * object type 0x6C, which ffmpeg, VLC, mpv and QuickTime play; browsers do not). No audio, no fragments: the frames are
 * appended to a `mdat` box as they arrive and the `moov` box with the sample tables is written by lm_mp4_close(), so a recording
 * that was never closed (crash, power loss) is not playable. The file is created with mode 0600 (a screen shows secrets).
 * Frame times are in milliseconds (the track's time scale is 1000); a frame lasts until the next one. Owner: mp4.c.
 */

#include <stddef.h>
#include <stdint.h>

struct lm_mp4;

/* Creates `path` (it must not exist). NULL on failure (errno is set). */
struct lm_mp4 *lm_mp4_create(const char *path, int width, int height);
/* Appends one JPEG picture shown from `pts_ms` on (milliseconds since any start; must not go backwards). 0 or -1 (disk full ...). */
int lm_mp4_add(struct lm_mp4 *m, const uint8_t *jpeg, size_t len, uint64_t pts_ms);
/* Number of pictures added so far / bytes written so far. */
uint32_t lm_mp4_frames(const struct lm_mp4 *m);
uint64_t lm_mp4_bytes(const struct lm_mp4 *m);
/* Writes the index; the last picture lasts until `end_ms`. Closes and frees `m` (also on failure, then the file is removed).
 * Returns 0, or -1. A file without any picture is removed too (-1). */
int lm_mp4_close(struct lm_mp4 *m, uint64_t end_ms);

#endif
