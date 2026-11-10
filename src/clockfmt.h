#ifndef MELLAGUI_CLOCKFMT_H
#define MELLAGUI_CLOCKFMT_H

/*
 * How the clock and the date are written: the panel's clock, the lock screen and the calendar program share this. No locale is used
 * (the names are English, fixed), so a test and a real session show the same. Settings: clock-hours, clock-seconds, clock-date,
 * clock-weekday, date-format (settings.c). Owner: clockfmt.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

enum { LM_DATE_DMY, LM_DATE_YMD, LM_DATE_MDY, LM_DATE_LONG, LM_DATE_FORMATS };

struct lm_clockfmt {
    bool h12;               /* 12 hour clock with AM / PM (default 24 hours) */
    bool seconds;           /* ... with seconds */
    bool show_date;         /* the panel shows the date before the time (default yes) */
    bool weekday;           /* a short weekday name before the date */
    int date_fmt;           /* LM_DATE_DMY 10.11.2026 (default), LM_DATE_YMD 2026-11-10, LM_DATE_MDY 11/10/2026, LM_DATE_LONG 10 Nov 2026 */
};

/* "14:05", "14:05:09", "2:05 PM", "2:05:09 PM". */
void lm_clock_time_text(const struct lm_clockfmt *f, const struct tm *tm, char *out, size_t n);
/* The date as date_fmt says, with the short weekday in front when f->weekday ("Tue 10.11.2026"). */
void lm_clock_date_text(const struct lm_clockfmt *f, const struct tm *tm, char *out, size_t n);
/* What the panel shows: the date (when show_date), a space, the time. At most 40 bytes with the buffer given. */
void lm_clock_bar_text(const struct lm_clockfmt *f, const struct tm *tm, char *out, size_t n);

const char *lm_weekday_name(int wday, bool full);       /* 0 = Sunday ("Sun" / "Sunday") */
const char *lm_month_name(int mon, bool full);          /* 0 = January */
const char *lm_date_format_name(int fmt);               /* "dmy" "ymd" "mdy" "long" */
int lm_date_format_parse(const char *name);             /* -1 when it is none of them */

/* Days in the month (mon 0..11, the full year, e.g. 2026) and the weekday of its first day (0 = Sunday). */
int lm_days_in_month(int year, int mon);
int lm_first_weekday(int year, int mon);
/* ISO 8601 week number (1..53) of a day. */
int lm_iso_week(int year, int mon, int mday);

#endif
