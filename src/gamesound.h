/*
 * gamesound.h - the sounds of the block game (mellagui-blocks): blocks broken, dug and put, the sword, goblins, the keep. All of them are made in code from noise,
 * tones and a small voice model: there are no sound files.
 *
 * A sound is asked for by the name of its family ("break-stone"); a family has one or more variants that differ a little, so that the same thing done twice does not
 * sound the same. sound.c plays them (ids LM_SOUND_COUNT + index), settings.c takes the names (write-only setting game-sound).
 */
#ifndef MELLAGUI_GAMESOUND_H
#define MELLAGUI_GAMESOUND_H

#include <stddef.h>
#include <stdint.h>

int lm_gamesound_count(void);                   /* variants of all families together */
const char *lm_gamesound_name(int i);           /* "game-break-stone", "game-break-stone2" ...; NULL when i is out of range */
/* The variant for `what`: a family name picks one of its variants (not the one it picked the time before), the family name with a number 1.. asks for that one
 * ("break-stone2"). -1 = there is no such sound. `seed` is anything that changes (the time). Main thread only. */
int lm_gamesound_pick(const char *what, uint32_t seed);
/* The samples (S16_LE mono 44.1 kHz) of variant i into buf, at most cap; returns how many (0 = no such sound). Not reentrant: one thread at a time. */
size_t lm_gamesound_render(int i, int16_t *buf, size_t cap);

#endif
