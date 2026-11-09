#ifndef MELLAGUI_FFPIPE_H
#define MELLAGUI_FFPIPE_H

/*
 * Optional H.264 through ffmpeg for mellagui-record: when `ffmpeg` with libx264 is installed, the recorder can feed it raw pictures
 * through a pipe instead of coding Motion JPEG itself. Nothing in MellaGUI depends on it; without ffmpeg the recorder keeps its own
 * encoders.
 *
 * The pictures are written as they change (XRGB8888 = ffmpeg's "bgr0"); ffmpeg stamps each with the time it was read
 * (-use_wallclock_as_timestamps), so the file has a variable frame rate and a still screen costs nothing. The output is a fragmented
 * MP4 (a recording that is cut short, even by a crash, still plays), H.264 yuv420p tagged BT.709, which browsers play.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ffpipe;

/* The ffmpeg to use for decoding (any build): $MELLAGUI_FFMPEG, else the first one in $PATH; NULL when there is none ("none" switches it off). */
const char *ffpipe_ffmpeg_path(void);

/* Is there an ffmpeg in $PATH that has libx264? (looked up once) */
bool ffpipe_available(void);

/* Starts ffmpeg writing `path` (it must exist already: the file keeps its mode). `crf` 0..51 (lower is better; 23 is x264's default, 20
 * suits text). NULL with a short message in err on failure. */
struct ffpipe *ffpipe_start(const char *path, int width, int height, int crf, char *err, size_t errsz);
/* Writes one picture (width * height * 4 bytes). 0, or -1 when ffmpeg has gone away. */
int ffpipe_write(struct ffpipe *p, const void *px, size_t bytes);
/* Closes the pipe, waits for ffmpeg to finish the file and frees `p`. 0 when ffmpeg ended well. */
int ffpipe_finish(struct ffpipe *p);

#endif
