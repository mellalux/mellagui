#ifndef MELLAGUI_AUDIOCAP_H
#define MELLAGUI_AUDIOCAP_H

/*
 * Sound for the recordings that are made without ffmpeg (the built-in H.264 / Motion JPEG writers of mellagui-record and the camera's fallback): `parec`
 * (PulseAudio's recorder, which PipeWire's pulse server serves too) is started for the computer's own sound (`@DEFAULT_MONITOR@`) and / or the microphone
 * (`@DEFAULT_SOURCE@`) and writes raw 44.1 kHz stereo 16 bit PCM; a thread per source reads it into a ring, `audiocap_read` hands out the mix. The caller puts
 * the PCM into the file (src/mp4.c, lm_mp4_add_audio). With ffmpeg none of this is used (client/ffpipe.c lets ffmpeg open the sources itself).
 *
 * A source is "pulse:DEVICE" (the same notation as ffpipe's: ffpipe_audio_source()); any other format is something only ffmpeg can read and is refused.
 * `$MELLAGUI_PAREC` names another program than `parec` (tests).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUDIOCAP_RATE 44100
#define AUDIOCAP_CHANNELS 2

struct audiocap;

/* Is the recorder program there (`parec`, or $MELLAGUI_PAREC)? */
bool audiocap_available(void);
/* Starts the sources that are not NULL. NULL with a message in err when none could be started. */
struct audiocap *audiocap_start(const char *system_src, const char *mic_src, char *err, size_t en);
/* The time (lm_now_ms()) at which the first sound that was read was heard; 0 while nothing has arrived. */
uint64_t audiocap_first_ms(const struct audiocap *a);
/* Up to `max_frames` frames of the mix (interleaved stereo), never waiting. 0 when nothing new has arrived. */
size_t audiocap_read(struct audiocap *a, int16_t *out, size_t max_frames);
/* Is some source still delivering? (false once every parec has ended: the sound server went away) */
bool audiocap_alive(const struct audiocap *a);
void audiocap_stop(struct audiocap *a);

#endif
