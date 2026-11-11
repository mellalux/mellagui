#ifndef MELLAGUI_CHIME_H
#define MELLAGUI_CHIME_H

/*
 * The start-up chime: an original little tune of about four seconds that is synthesised in code (no sound files), meant to feel like the
 * two moods the author asked for - the cheeky, bouncy, Italian-sounding side of an espresso (a mandolin that plays a bouncing arpeggio over an
 * "oom-pah" of bass and accordion chords) and the warm Estonian ballad side (a piano that plays Am - F - G - C under a slowly falling line
 * E - D - B - C). It quotes neither song: the notes are our own.
 *
 * Mono, signed 16 bit, 44100 Hz. lm_chime_render() is pure (no I/O): it fills `out` (at most `cap` samples) and returns the number of
 * samples written, 0 when `cap` is too small. lm_sound_startup() (sound.h) plays it.
 */

#include <stddef.h>
#include <stdint.h>

#define LM_CHIME_RATE 44100
#define LM_CHIME_MAX_SAMPLES (LM_CHIME_RATE * 6)          /* the most it will ever need */

size_t lm_chime_render(int16_t *out, size_t cap);

#endif
