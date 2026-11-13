#ifndef MELLAGUI_SCREENOFF_H
#define MELLAGUI_SCREENOFF_H

/*
 * Putting the monitors to sleep smoothly. The one place that does it, for the screen lock (lock.c, "Screen off after") and for the
 * screensaver (saver.c, `saver-blank`): the picture fades to black over about 2 seconds (the backend's brightness, lm_backend_set_dim:
 * the CRTC gamma tables on DRM, so it costs nothing per frame and screenshots stay bright) and only then the monitors go to standby
 * (DPMS, lm_backend_set_power). The first input wakes them (lm_screenoff_wake): the picture fades back in at once, 0.3 s, and that
 * input is swallowed by the seat, like the one that wakes a locked screen.
 *
 * Tests: MELLAGUI_BLANK_FADE_MS replaces the 2 s (and the 0.3 s fade in is a tenth of it, at least 30 ms).
 */

#include <stdbool.h>

struct lm_server;
struct lm_screenoff;

struct lm_screenoff *lm_screenoff_create(struct lm_server *srv);
void lm_screenoff_destroy(struct lm_screenoff *s);

/* Begin the fade out (no-op while it is fading or the monitors are already off). */
void lm_screenoff_start(struct lm_screenoff *s);
/* Bring the screen back (cancels a fade, switches the monitors on, fades in). Returns true when it was fading or off: the input that
 * caused it is only a wake-up call and must not reach anything else. */
bool lm_screenoff_wake(struct lm_screenoff *s);
/* Fading out, or the monitors are off. */
bool lm_screenoff_active(const struct lm_screenoff *s);
/* The monitors are off now (the fade is over). */
bool lm_screenoff_off(const struct lm_screenoff *s);
/* How bright the display is now, 0..1 (the control socket's `screenoff`). */
double lm_screenoff_level(const struct lm_screenoff *s);

#endif
