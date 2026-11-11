/*
 * sound.h - short feedback beeps (sticky keys). No audio library: the tones are synthesised into a small
 * S16_LE mono buffer and piped to `paplay` (default device, via the sound server) or `aplay -D` (a chosen ALSA device) in a detached child; when both are missing or
 * there is no sound card, nothing happens. The child is reaped by the server's SIGCHLD watch.
 */
#ifndef MELLAGUI_SOUND_H
#define MELLAGUI_SOUND_H

enum lm_sound {
    LM_SOUND_STICKY_ON,     /* sticky keys switched on: rising three-note run */
    LM_SOUND_STICKY_OFF,    /* sticky keys switched off: falling three-note run */
    LM_SOUND_LATCH,         /* modifier latched for the next key: one short beep */
    LM_SOUND_LOCK,          /* modifier locked: two higher beeps */
    LM_SOUND_UNLOCK,        /* modifier unlocked: one low beep */
    LM_SOUND_SHOT,          /* screenshot taken: a short falling pair */
    LM_SOUND_DOCK,          /* a click on a dock icon: a soft rising "pok-pling" (plucked, with a round decay) */
    LM_SOUND_VOLUME,        /* the volume was set from the quick settings card: a short plucked tick, so that the new level can be heard */
    LM_SOUND_COUNT
};

const char *lm_sound_name(enum lm_sound s);     /* "latch", "lock", ... */
/* ALSA device the beeps go to (aplay -D): "default" or a name accepted by lm_audio_name_ok(). */
void lm_sound_set_device(const char *name);
/* Plays `s` asynchronously. Never blocks, never fails loudly. */
void lm_sound_play(enum lm_sound s);
/*
 * The start-up chime (chime.h, about 4.7 s). Everything - synthesis, playing - happens in a detached child, so the compositor does not wait:
 * the child renders the tune into a memfd (it is far bigger than a pipe holds), starts paplay / aplay on it and, when that fails because the
 * sound server is not up yet (the compositor starts at login, PipeWire a moment later), tries again every half second for ~6 s.
 * MELLAGUI_CHIME_WAV=FILE writes the tune there as a WAV file instead of playing it (tests).
 */
void lm_sound_startup(void);
/* Wakes the sound server's sink with a few seconds of silence (nothing is heard): called when the pointer reaches the dock, so that the click sound
 * that follows is not eaten by an HDMI output waking up. Rate limited; a no-op for a bare ALSA device. */
void lm_sound_wake(void);
/* Records `s` as the last interface sound (tests read it from the control socket `sound`, also on the headless backend where nothing is played). */
void lm_sound_note(enum lm_sound s);
const char *lm_sound_last(void);                /* name of the last lm_sound_note(), "none" before the first */
unsigned lm_sound_noted(void);                  /* how many there were */

#endif
