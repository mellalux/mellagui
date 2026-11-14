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
 *     night-sun    on | off     with "auto": from sunset until sunrise of the day instead of the two times (src/smartsun.h works the times out
 *                               from the date and the place - $MELLAGUI_SMART_PLACE, smart-place.conf, else the time zone's; $MELLAGUI_SMART_SUN
 *                               "HH:MM,HH:MM" replaces the calculation in tests); the two times are used while the place is not known
 *     night-sun-info (read only) "HH:MM|HH:MM|PLACE" = today's sunrise, sunset and the place, "" when there is none
 *   key action `night-light` (Super+Shift+N): switches between "on" and "off" (it replaces "auto" until the setting is changed again).
 *
 * Changes fade in: about 1.5 s when the user switches it, 30 s when the schedule says so. The level (0..1) goes from no tint to the
 * temperature's gains. Tests: MELLAGUI_NIGHT_NOW=HH:MM replaces the clock, MELLAGUI_NIGHT_FADE_MS=N replaces both fade times.
 */

#include <stdbool.h>
#include <stddef.h>

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

/* The span a scheduled night light covers today, in minutes after midnight (-1 = not a time): sunset .. sunrise when night-sun is on and the
 * sun is known, else night-from .. night-to. */
void lm_night_span(struct lm_night *n, int *from, int *to);
/* "HH:MM|HH:MM|PLACE" (today's sunrise, sunset and where from; blanks of the place are written as "_"), or "" when the place is not known or the sun
 * does not rise / set today. Read-only setting night-sun-info. */
void lm_night_sun_text(struct lm_night *n, char *out, size_t size);

/* Pure helpers (also used by the unit test): the red / green / blue gains of a colour temperature (1.0 at 6500 K and above). */
void lm_night_gains(int kelvin, double rgb[3]);
/* "HH:MM" -> minutes after midnight, -1 when it is not a time. */
int lm_night_parse_time(const char *text);
/* Is `now` (minutes) inside from..to? The span crosses midnight when from > to; from == to is never. */
bool lm_night_in_span(int now, int from, int to);

#endif
