#ifndef MELLAGUI_NIGHTLIGHT_H
#define MELLAGUI_NIGHTLIGHT_H

/*
 * Night light: the whole display gets a warmer colour (less blue) in the evening. It is the backend's job to show it
 * (lm_backend_set_tint: DRM puts the gains in the CRTCs' gamma tables, so it costs nothing per frame and screenshots / the screen
 * recorder stay untinted; headless tints the pictures `dump` writes). This module only decides *how much*, when:
 *
 *   settings (settings.conf, Display page of the panel):
 *     night-light  off | on | auto      auto = between night-from and night-to (local time, "HH:MM"; the span may cross midnight)
 *     night-temp   1500..6500 K         the colour temperature at full strength (6500 = no change, lower = warmer)
 *     night-from, night-to
 *   key action `night-light` (Super+Shift+N): switches between "on" and "off" (it replaces "auto" until the setting is changed again).
 *
 * Changes fade in: about 1.5 s when the user switches it, 30 s when the schedule says so. The level (0..1) goes from no tint to the
 * temperature's gains. Tests: MELLAGUI_NIGHT_NOW=HH:MM replaces the clock, MELLAGUI_NIGHT_FADE_MS=N replaces both fade times.
 */

#include <stdbool.h>

struct lm_server;
struct lm_night;

struct lm_night *lm_night_create(struct lm_server *srv);
void lm_night_destroy(struct lm_night *n);

/* A night-* setting changed (or was loaded): looks at the schedule again and fades to the new target. */
void lm_night_changed(struct lm_night *n);
/* The key action: on <-> off. */
void lm_night_toggle(struct lm_night *n);

/* Is the night light on right now (target, not the fade's position)? level = how far the fade has come, 0..1. */
bool lm_night_active(const struct lm_night *n);
double lm_night_level(const struct lm_night *n);

/* Pure helpers (also used by the unit test): the red / green / blue gains of a colour temperature (1.0 at 6500 K and above). */
void lm_night_gains(int kelvin, double rgb[3]);
/* "HH:MM" -> minutes after midnight, -1 when it is not a time. */
int lm_night_parse_time(const char *text);
/* Is `now` (minutes) inside from..to? The span crosses midnight when from > to; from == to is never. */
bool lm_night_in_span(int now, int from, int to);

#endif
