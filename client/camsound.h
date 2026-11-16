#ifndef MELLAGUI_CAMSOUND_H
#define MELLAGUI_CAMSOUND_H

/*
 * camsound - the sounds of mellagui-cam, made in code (no sound files): the tick of the countdown, a higher one for the last second, and the click of a
 * shutter (two bursts of noise and a soft thump, like a mirror and a curtain). Mono, signed 16 bit, CAMSOUND_RATE Hz.
 *
 * camsound_play() plays one in a detached child (paplay, else aplay; $MELLAGUI_CAM_PLAYER = another program that reads raw S16_LE mono 44100 Hz on its
 * standard input, `none` = silent) and never blocks. $MELLAGUI_CAM_SOUND_LOG=FILE appends the name of every sound there ("tick", "tock", "shutter")
 * instead of playing it (the tests).
 */

#include <stddef.h>
#include <stdint.h>

#define CAMSOUND_RATE 44100
#define CAMSOUND_MAX (CAMSOUND_RATE / 4)         /* at most a quarter of a second */

enum { CAMSOUND_TICK, CAMSOUND_TOCK, CAMSOUND_SHUTTER, CAMSOUND_COUNT };

const char *camsound_name(int kind);
size_t camsound_render(int kind, int16_t *buf, size_t cap);      /* the number of samples (0 for an unknown kind or too little room) */
void camsound_play(int kind);

#endif
