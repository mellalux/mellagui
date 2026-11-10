#include "clockfmt.h"

#include <stdio.h>
#include <string.h>

static const char *const wd_short[7] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const wd_full[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const mon_short[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const mon_full[12] = { "January", "February", "March", "April", "May", "June", "July", "August", "September",
                                          "October", "November", "December" };
static const char *const fmt_names[LM_DATE_FORMATS] = { "dmy", "ymd", "mdy", "long" };

const char *lm_weekday_name(int wday, bool full)
{
    if (wday < 0 || wday > 6)
        return "";
    return full ? wd_full[wday] : wd_short[wday];
}

const char *lm_month_name(int mon, bool full)
{
    if (mon < 0 || mon > 11)
        return "";
    return full ? mon_full[mon] : mon_short[mon];
}

const char *lm_date_format_name(int fmt)
{
    return fmt >= 0 && fmt < LM_DATE_FORMATS ? fmt_names[fmt] : fmt_names[0];
}

int lm_date_format_parse(const char *name)
{
    for (int i = 0; i < LM_DATE_FORMATS; i++)
        if (name && !strcmp(name, fmt_names[i]))
            return i;
    return -1;
}

void lm_clock_time_text(const struct lm_clockfmt *f, const struct tm *tm, char *out, size_t n)
{
    int h = tm->tm_hour;
    const char *suffix = "";
    char sec[8] = "";
    if (f->seconds)
        snprintf(sec, sizeof(sec), ":%02d", tm->tm_sec);
    if (f->h12) {
        suffix = h >= 12 ? " PM" : " AM";
        h %= 12;
        if (h == 0)
            h = 12;
        snprintf(out, n, "%d:%02d%s%s", h, tm->tm_min, sec, suffix);
    } else {
        snprintf(out, n, "%02d:%02d%s", h, tm->tm_min, sec);
    }
}

void lm_clock_date_text(const struct lm_clockfmt *f, const struct tm *tm, char *out, size_t n)
{
    int d = tm->tm_mday, m = tm->tm_mon + 1, y = (tm->tm_year + 1900) % 10000;
    char date[32];
    switch (f->date_fmt) {
    case LM_DATE_YMD: snprintf(date, sizeof(date), "%04d-%02d-%02d", y, m, d); break;
    case LM_DATE_MDY: snprintf(date, sizeof(date), "%02d/%02d/%04d", m, d, y); break;
    case LM_DATE_LONG: snprintf(date, sizeof(date), "%d %s %04d", d, lm_month_name(tm->tm_mon, false), y); break;
    default: snprintf(date, sizeof(date), "%02d.%02d.%04d", d, m, y); break;
    }
    if (f->weekday)
        snprintf(out, n, "%s %s", lm_weekday_name(tm->tm_wday, false), date);
    else
        snprintf(out, n, "%s", date);
}

void lm_clock_bar_text(const struct lm_clockfmt *f, const struct tm *tm, char *out, size_t n)
{
    char t[24], d[48];
    lm_clock_time_text(f, tm, t, sizeof(t));
    if (!f->show_date) {
        snprintf(out, n, "%s", t);
        return;
    }
    lm_clock_date_text(f, tm, d, sizeof(d));
    snprintf(out, n, "%s %s", d, t);
}

int lm_days_in_month(int year, int mon)
{
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mon < 0 || mon > 11)
        return 30;
    if (mon == 1 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return dm[mon];
}

/* Sakamoto's method: 0 = Sunday. */
static int weekday_of(int year, int mon, int mday)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    int y = year - (mon < 2);
    return (y + y / 4 - y / 100 + y / 400 + t[mon] + mday) % 7;
}

int lm_first_weekday(int year, int mon)
{
    return weekday_of(year, mon < 0 ? 0 : mon > 11 ? 11 : mon, 1);
}

static int day_of_year(int year, int mon, int mday)
{
    int n = mday;
    for (int m = 0; m < mon; m++)
        n += lm_days_in_month(year, m);
    return n;                                                   /* 1-based */
}

int lm_iso_week(int year, int mon, int mday)
{
    int wd = weekday_of(year, mon, mday);
    int iso_wd = wd == 0 ? 7 : wd;                              /* Monday = 1 */
    int week = (day_of_year(year, mon, mday) - iso_wd + 10) / 7;
    if (week < 1) {                                             /* the last week of the year before */
        int py = year - 1;
        int dec31_wd = weekday_of(py, 11, 31);
        int dec31_iso = dec31_wd == 0 ? 7 : dec31_wd;
        return (day_of_year(py, 11, 31) - dec31_iso + 10) / 7;
    }
    if (week == 53) {                                           /* it may be week 1 of the next year */
        int jan1 = weekday_of(year + 1, 0, 1);
        int jan1_iso = jan1 == 0 ? 7 : jan1;
        if (jan1_iso <= 4)                                      /* Jan 1 is Mon..Thu: week 1 of next year starts this week */
            return 1;
    }
    return week;
}
