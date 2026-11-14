/*
 * meow: a cat's meow made in code (no sound files), for Tom, the cat of the Matrix room (client/matrixcat.c).
 *
 * A voice model in the usual way: a glottal source (a pulse train with a little jitter, vibrato and breath noise) whose pitch rises and falls, through three
 * resonators (formants) that move from "m" (nasal, quiet) through "ee" and "a" to "oo" - which is what makes it "mee-aow". `lm_meow_render` is deterministic for
 * a seed (every meow of a day is a little different: the length, the pitch, how high it rises). Mono, signed 16 bit, `LM_MEOW_RATE` Hz.
 */
#ifndef LM_MEOW_H
#define LM_MEOW_H

#include <stddef.h>
#include <stdint.h>

#define LM_MEOW_RATE 44100
#define LM_MEOW_MAX (LM_MEOW_RATE * 3 / 2)      /* at most 1.5 s */

/* Writes one meow into buf (room for `cap` samples, at least LM_MEOW_MAX) and returns the number of samples. */
size_t lm_meow_render(int16_t *buf, size_t cap, uint32_t seed);

/* Plays one meow of that seed in a detached child (paplay, else aplay; `MELLAGUI_MEOW_PLAYER` = another program that reads raw S16_LE mono 44100 Hz on its
 * standard input, `none` = silent; `MELLAGUI_MEOW_WAV=FILE` writes a WAV file there instead). Never blocks, never fails loudly. */
void lm_meow_play(uint32_t seed);

/* Writes the meow as a WAV file (the test and `mellagui-matrix --meow-wav`); true on success. */
int lm_meow_write_wav(const char *path, uint32_t seed);

#endif
