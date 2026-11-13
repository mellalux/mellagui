#ifndef MELLAGUI_SMARTSCHED_H
#define MELLAGUI_SMARTSCHED_H

/*
 * smartsched - mellagui-smart's own timetable: "at 07:30 on weekdays tell the blind position 30". The rules are lines of
 * $XDG_CONFIG_HOME/mellagui/smart-schedule.conf:
 *
 *     on 07:30 MTWTF-- DEVICE_ID percent_control 30        (on | off, the time, the seven days from Monday with '-' for "not", device, code, value)
 *     on sunrise+15 MTWTFSS DEVICE_ID control open         (the time can be `sunrise` or `sunset`, with minutes before (-) or after (+): smartsun.h)
 *
 * One window-less process carries them out (`mellagui-smart --background`, started with the compositor and by the window when a rule is saved; it
 * holds a lock in $XDG_RUNTIME_DIR, so there is only one, and ends when no rule is on or the Wayland session is gone). It looks at the clock every
 * 15 s and sends a rule's command through the cloud in the minute it is due - so it needs this computer to be on and awake at that minute; a missed
 * minute is not made up for. A command that fails is tried three times, then a notification says so.
 * $MELLAGUI_SMART_NOW=HH:MM stands in for the clock and $MELLAGUI_SMART_TICK_MS for the 15 s (tests).
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#define SCHED_MAX 64

struct sched_rule {
    bool on;
    int hour, min;                      /* the time of day (when `sun` is 0) */
    int sun;                            /* 0 = at hour:min, 1 = at sunrise, 2 = at sunset ... */
    int offset;                         /* ... plus this many minutes (-180..180) */
    unsigned days;                      /* bit 0 = Monday ... bit 6 = Sunday */
    char dev[40], code[40], value[24];
};

int sched_load(struct sched_rule *r, int max);
bool sched_save(const struct sched_rule *r, int n);
/* "07:30", "sunrise", "sunset + 15 min" (spaces as the window shows it). */
void sched_time_text(const struct sched_rule *r, char *out, size_t n);
/* The minute of the day (0..1439) the rule is due on the day that holds `when`; false when it has none that day (the sun does not rise, the place is
 * not known, the offset takes it out of the day). */
bool sched_minute(const struct sched_rule *r, time_t when, int *minute);
/* "every day", "weekdays", "weekends" or "Mon Wed Fri". */
void sched_days_text(unsigned days, char *out, size_t n);
/* One line of the file: false when it is not a rule. */
bool sched_parse(const char *line, struct sched_rule *r);
/* The loop of the window-less process; returns its exit status. */
int sched_background(void);
/* Starts that process (it ends at once when another one runs). Not when $MELLAGUI_SMART_NO_BACKGROUND is set (tests). */
void sched_spawn_background(void);
int sched_selftest(void);

#endif
