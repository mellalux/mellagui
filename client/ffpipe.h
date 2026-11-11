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
 *
 * Sound: ffmpeg itself opens the sound sources (`-f pulse`, which the PipeWire pulse server serves too), so the sound is stamped by the same wall clock
 * as the pictures and stays in step with them. Up to two sources (the computer's own sound = the monitor of the default output, and the microphone) are
 * mixed into one AAC track. A source is written "format:device": the defaults are `pulse:@DEFAULT_MONITOR@` and `pulse:@DEFAULT_SOURCE@`;
 * `$MELLAGUI_RECORD_SYSTEM_SRC` / `$MELLAGUI_RECORD_MIC_SRC` replace them (tests: `lavfi:sine=frequency=440`), `none` switches a source off.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct ffpipe;

struct ffpipe_audio {
    const char *system;                 /* NULL = not recorded, else a source ("format:device"); see ffpipe_audio_source() */
    const char *mic;
};

/* The source to use for the computer's sound (mic = false) or the microphone (mic = true): the environment's, else the default one.
 * NULL when the environment says `none`. The pointer stays valid. */
const char *ffpipe_audio_source(bool mic);

/* The ffmpeg to use for decoding (any build): $MELLAGUI_FFMPEG, else the first one in $PATH; NULL when there is none ("none" switches it off). */
const char *ffpipe_ffmpeg_path(void);

/* Is there an ffmpeg in $PATH that has libx264? (looked up once) */
bool ffpipe_available(void);

/* Starts ffmpeg writing `path` (it must exist already: the file keeps its mode). `crf` 0..51 (lower is better; 23 is x264's default, 20
 * suits text). `audio` may be NULL (no sound). NULL with a short message in err on failure. */
struct ffpipe *ffpipe_start(const char *path, int width, int height, int crf, const struct ffpipe_audio *audio, char *err, size_t errsz);
/* Writes one picture (width * height * 4 bytes). 0, or -1 when ffmpeg has gone away. Waits while the pipe is full. */
int ffpipe_write(struct ffpipe *p, const void *px, size_t bytes);
/* The same without ever waiting (a window that must stay alive, a camera that makes pictures faster than x264 takes them): 0 = taken, 1 = ffmpeg is still
 * busy with the one before, the picture is dropped (the file just keeps the earlier one a little longer), -1 = ffmpeg has gone away. */
int ffpipe_offer(struct ffpipe *p, const void *px, size_t bytes);
/* Closes the pipe, waits for ffmpeg to finish the file and frees `p`. 0 when ffmpeg ended well. */
int ffpipe_finish(struct ffpipe *p);

#endif
