/*
 * sound.h - short feedback beeps (sticky keys). No audio library: the tones are synthesised into a small
 * S16_LE mono buffer and piped to `aplay` (alsa-utils) in a detached child; when aplay is missing or
 * there is no sound card, nothing happens. The child is reaped by the server's SIGCHLD watch.
 */
#ifndef LUMEN_SOUND_H
#define LUMEN_SOUND_H

enum lm_sound {
    LM_SOUND_STICKY_ON,     /* sticky keys switched on: rising three-note run */
    LM_SOUND_STICKY_OFF,    /* sticky keys switched off: falling three-note run */
    LM_SOUND_LATCH,         /* modifier latched for the next key: one short beep */
    LM_SOUND_LOCK,          /* modifier locked: two higher beeps */
    LM_SOUND_UNLOCK,        /* modifier unlocked: one low beep */
    LM_SOUND_COUNT
};

const char *lm_sound_name(enum lm_sound s);     /* "latch", "lock", ... */
/* Plays `s` asynchronously. Never blocks, never fails loudly. */
void lm_sound_play(enum lm_sound s);

#endif
