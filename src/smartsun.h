#ifndef MELLAGUI_SMARTSUN_H
#define MELLAGUI_SMARTSUN_H

/*
 * smartsun - when the sun rises and sets, for the timetable's "at sunrise" / "at sunset" rules (smartsched.h). The times are worked out here from the
 * date and the place (the series of the NOAA solar calculator: the equation of time and the sun's declination from the day of the year, the hour angle
 * of the sun's upper limb at 90.833 degrees; good to about a minute away from the polar circles), no network and no tables.
 *
 * The place: $MELLAGUI_SMART_PLACE ("LAT,LON" in degrees, east positive; tests) or the file $XDG_CONFIG_HOME/mellagui/smart-place.conf (one line
 * "LAT LON", written by `mellagui-smart --place LAT LON`), else the one of the computer's time zone: its name ($TZ, or where /etc/localtime points)
 * is looked up in the zone tables of the system (/usr/share/zoneinfo/zone1970.tab and zone.tab, coordinates in ISO 6709) - for Europe/Tallinn that is
 * the middle of Tallinn, close enough for a blind. $MELLAGUI_SMART_SUN="HH:MM,HH:MM" replaces the calculation (sunrise, sunset of every day: tests).
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* The place to work for: degrees (north, east positive) and a short description ("Europe/Tallinn", "chosen by you", ...). false when nothing says. */
bool sun_place(double *lat, double *lon, char *name, size_t n);
/* Writes the file for "mellagui-smart --place LAT LON". */
bool sun_place_save(double lat, double lon);
/* Sunrise and sunset of the local day that contains `when`, in minutes since local midnight (0..1439). false when the sun does not rise or does not
 * set that day (polar day or night). */
bool sun_events(time_t when, double lat, double lon, int *rise, int *set);
/* The same for the place sun_place() finds; false when there is none or the sun does not rise. */
bool sun_today(time_t when, int *rise, int *set);
/* "+5925+02445" or "+492719-0123456" (ISO 6709 as in zone1970.tab) -> degrees. */
bool sun_parse_iso6709(const char *s, double *lat, double *lon);
int sun_selftest(void);

#endif
