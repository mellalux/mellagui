#include "smartsun.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool place_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    int w;
    if (xdg && *xdg)
        w = snprintf(out, n, "%s/mellagui/smart-place.conf", xdg);
    else if (home && *home)
        w = snprintf(out, n, "%s/.config/mellagui/smart-place.conf", home);
    else
        return false;
    return w > 0 && (size_t)w < n;
}

/* One signed number of DDMM / DDDMM / DDMMSS / DDDMMSS written as digits: `digits` of them after the sign, the first `deg` are degrees. */
static bool iso_part(const char *s, int deg, int *used, double *out)
{
    if (s[0] != '+' && s[0] != '-')
        return false;
    int n = 0;
    while (s[1 + n] >= '0' && s[1 + n] <= '9')
        n++;
    if (n != deg + 2 && n != deg + 4)
        return false;
    double v = 0;
    for (int i = 0; i < deg; i++)
        v = v * 10 + (s[1 + i] - '0');
    double min = (s[1 + deg] - '0') * 10 + (s[2 + deg] - '0'), sec = 0;
    if (n == deg + 4)
        sec = (s[3 + deg] - '0') * 10 + (s[4 + deg] - '0');
    v += min / 60.0 + sec / 3600.0;
    *out = s[0] == '-' ? -v : v;
    *used = 1 + n;
    return true;
}

bool sun_parse_iso6709(const char *s, double *lat, double *lon)
{
    int u1 = 0, u2 = 0;
    double a, b;
    if (!iso_part(s, 2, &u1, &a) || !iso_part(s + u1, 3, &u2, &b) || fabs(a) > 90 || fabs(b) > 180)
        return false;
    *lat = a;
    *lon = b;
    return true;
}

/* The name of the computer's time zone ("Europe/Tallinn"): $TZ, else where /etc/localtime points. */
static bool zone_name(char *out, size_t n)
{
    const char *tz = getenv("TZ");
    char link[512];
    if (tz && *tz) {
        if (*tz == ':')
            tz++;
        const char *z = strstr(tz, "zoneinfo/");
        snprintf(out, n, "%s", z ? z + 9 : tz);
        return true;
    }
    ssize_t k = readlink("/etc/localtime", link, sizeof(link) - 1);
    if (k <= 0)
        return false;
    link[k] = 0;
    const char *z = strstr(link, "zoneinfo/");
    if (!z)
        return false;
    snprintf(out, n, "%s", z + 9);
    return true;
}

static bool zone_place(const char *zone, double *lat, double *lon)
{
    static const char *const tabs[2] = { "/usr/share/zoneinfo/zone1970.tab", "/usr/share/zoneinfo/zone.tab" };
    for (int t = 0; t < 2; t++) {
        FILE *f = fopen(tabs[t], "r");
        if (!f)
            continue;
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char cc[128], coord[40], name[128];
            if (line[0] == '#' || sscanf(line, "%127s %39s %127s", cc, coord, name) != 3)
                continue;
            if (!strcmp(name, zone) && sun_parse_iso6709(coord, lat, lon)) {
                fclose(f);
                return true;
            }
        }
        fclose(f);
    }
    return false;
}

bool sun_place(double *lat, double *lon, char *name, size_t n)
{
    const char *env = getenv("MELLAGUI_SMART_PLACE");
    double a, b;
    if (env && *env) {
        if (sscanf(env, "%lf,%lf", &a, &b) == 2) {
            *lat = a;
            *lon = b;
            snprintf(name, n, "MELLAGUI_SMART_PLACE");
            return true;
        }
        return false;
    }
    char path[1024];
    if (place_path(path, sizeof(path))) {
        FILE *f = fopen(path, "r");
        if (f) {
            char line[200];
            bool ok = fgets(line, sizeof(line), f) && sscanf(line, "%lf %lf", &a, &b) == 2 && fabs(a) <= 90 && fabs(b) <= 180;
            fclose(f);
            if (ok) {
                *lat = a;
                *lon = b;
                snprintf(name, n, "chosen by you");
                return true;
            }
        }
    }
    char zone[128];
    if (zone_name(zone, sizeof(zone)) && zone_place(zone, lat, lon)) {
        snprintf(name, n, "%s", zone);
        return true;
    }
    return false;
}

bool sun_place_save(double lat, double lon)
{
    char path[1024];
    if (fabs(lat) > 90 || fabs(lon) > 180 || !place_path(path, sizeof(path)))
        return false;
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(path, 0700);
        *p = '/';
    }
    FILE *f = fopen(path, "w");
    if (!f)
        return false;
    fprintf(f, "%.4f %.4f\n", lat, lon);
    return fclose(f) == 0;
}

/* Minutes after UTC midnight of the given day (y, day of the year) at which the sun rises and sets; false at the poles in summer / winter. */
static bool utc_minutes(int doy, double lat, double lon, double *rise, double *set)
{
    double g = 2 * M_PI / 365.0 * (doy - 1);
    double eqtime = 229.18 * (0.000075 + 0.001868 * cos(g) - 0.032077 * sin(g) - 0.014615 * cos(2 * g) - 0.040849 * sin(2 * g));
    double decl = 0.006918 - 0.399912 * cos(g) + 0.070257 * sin(g) - 0.006758 * cos(2 * g) + 0.000907 * sin(2 * g) - 0.002697 * cos(3 * g) +
                  0.00148 * sin(3 * g);
    double latr = lat * M_PI / 180.0;
    double c = cos(90.833 * M_PI / 180.0) / (cos(latr) * cos(decl)) - tan(latr) * tan(decl);
    if (c > 1.0 || c < -1.0)
        return false;
    double ha = acos(c) * 180.0 / M_PI;
    *rise = 720.0 - 4.0 * (lon + ha) - eqtime;
    *set = 720.0 - 4.0 * (lon - ha) - eqtime;
    return true;
}

bool sun_events(time_t when, double lat, double lon, int *rise, int *set)
{
    const char *ov = getenv("MELLAGUI_SMART_SUN");
    int a, b, c, d;
    if (ov && *ov) {
        if (sscanf(ov, "%d:%d,%d:%d", &a, &b, &c, &d) != 4)
            return false;
        *rise = a * 60 + b;
        *set = c * 60 + d;
        return true;
    }
    struct tm tm, nl;
    localtime_r(&when, &tm);
    struct tm noon = tm;
    noon.tm_hour = 12;
    noon.tm_min = noon.tm_sec = 0;
    noon.tm_isdst = -1;
    time_t tn = mktime(&noon);
    localtime_r(&tn, &nl);
    double off = (double)nl.tm_gmtoff / 60.0, r, s;
    if (!utc_minutes(tm.tm_yday + 1, lat, lon, &r, &s))
        return false;
    int mr = (int)lround(r + off), ms = (int)lround(s + off);
    *rise = ((mr % 1440) + 1440) % 1440;                        /* (a place far from its zone's meridian may land on the next day: the same time of day) */
    *set = ((ms % 1440) + 1440) % 1440;
    return true;
}

bool sun_today(time_t when, int *rise, int *set)
{
    double lat = 0, lon = 0;
    char name[64];
    if (getenv("MELLAGUI_SMART_SUN") && *getenv("MELLAGUI_SMART_SUN"))
        return sun_events(when, 0, 0, rise, set);
    return sun_place(&lat, &lon, name, sizeof(name)) && sun_events(when, lat, lon, rise, set);
}

int sun_selftest(void)
{
    int bad = 0;
    double lat, lon, r, s;
#define CHECK(cond, what) do { if (!(cond)) { fprintf(stderr, "smartsun selftest: %s\n", what); bad++; } } while (0)
    CHECK(sun_parse_iso6709("+5925+02445", &lat, &lon) && fabs(lat - 59.4167) < 0.001 && fabs(lon - 24.75) < 0.001, "Tallinn's coordinates");
    CHECK(sun_parse_iso6709("-3352+15113", &lat, &lon) && lat < -33.8 && lat > -33.9 && lon > 151.2 && lon < 151.3, "Sydney's coordinates");
    CHECK(sun_parse_iso6709("+492719-0123456", &lat, &lon) && fabs(lat - 49.4553) < 0.001 && fabs(lon + 12.5822) < 0.001, "coordinates with seconds");
    CHECK(!sun_parse_iso6709("5925+02445", &lat, &lon) && !sun_parse_iso6709("+59+024", &lat, &lon) && !sun_parse_iso6709("+9925+02445", &lat, &lon), "bad coordinates");
    /* London on the longest day (21 June = day 172): 03:43 and 20:21 UTC; Tallinn on the shortest (21 Dec = day 355): 09:18 and 15:22 local = about
     * 07:18 and 13:22 UTC, a day of 6 h 04 min; on the longest (day 172): 04:04 and 22:42 local */
    CHECK(utc_minutes(172, 51.5074, -0.1278, &r, &s) && fabs(r - (3 * 60 + 43)) < 4 && fabs(s - (20 * 60 + 21)) < 4, "London at midsummer");
    CHECK(utc_minutes(355, 59.4167, 24.75, &r, &s) && fabs(r - (7 * 60 + 18)) < 4 && fabs(s - (13 * 60 + 22)) < 4, "Tallinn at midwinter");
    CHECK(utc_minutes(172, 59.4167, 24.75, &r, &s) && fabs(r - (1 * 60 + 4)) < 4 && fabs(s - (19 * 60 + 42)) < 4, "Tallinn at midsummer");
    CHECK(utc_minutes(80, 0.0, 0.0, &r, &s) && fabs(((r + s) / 2) - 727) < 4 && fabs((s - r) - 727) < 8, "the equinox on the equator: noon at 12:07, a day of 12 h 07 min");
    CHECK(!utc_minutes(172, 78.2, 15.6, &r, &s) && !utc_minutes(355, 78.2, 15.6, &r, &s), "Svalbard has a polar day and a polar night");
    CHECK(utc_minutes(172, 59.4167, 24.75, &r, &s) && (s - r) > 17 * 60 && (s - r) < 19 * 60, "a long day in Tallinn in June");
#undef CHECK
    return bad;
}
