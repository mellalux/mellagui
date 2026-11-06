/*
 * sysinfo.h - CPU load, memory and temperature for the system-monitor gadget. Plain file reads (/proc/stat,
 * /proc/meminfo, /sys/class/hwmon, /sys/class/thermal): small and fast, so they are done in the event loop without child
 * processes. Env LUMEN_SYS_ROOT prefixes every path (a fake /proc and /sys tree for the tests).
 */
#ifndef LM_SYSINFO_H
#define LM_SYSINFO_H

#include <stdbool.h>
#include <stddef.h>

#define LM_SYS_MAX_CORES 32
#define LM_SYS_HISTORY 60               /* samples kept for the CPU graph (one per second) */

struct lm_cpu_times {
    unsigned long long busy, total;     /* jiffies: busy = total - idle - iowait */
};

struct lm_sysinfo_state {
    bool ready;                         /* at least one sample */
    int cpu;                            /* overall load since the previous sample, percent */
    int ncores;
    int core[LM_SYS_MAX_CORES];         /* per logical CPU */
    int hist[LM_SYS_HISTORY];           /* overall load, oldest first, the last `nhist` entries are valid */
    int nhist;
    long mem_total_kb, mem_avail_kb;
    int mem_pct;                        /* used = total - available, percent */
    long swap_total_kb, swap_free_kb;
    int swap_pct;
    int temp_c;                         /* CPU package/core temperature in degrees C, -1 = no sensor */
    char temp_label[24];                /* which sensor ("coretemp Package id 0") */
    double load1;                       /* 1-minute load average */
};

/* ---- pure helpers (unit-tested) -------------------------------------------------------------------------------- */

/* /proc/stat: the "cpu " line into `total`, the "cpuN" lines into cores[]; returns the number of cores. */
int lm_sysinfo_parse_stat(const char *text, struct lm_cpu_times *total, struct lm_cpu_times *cores, int max);
/* Load between two samples, percent 0..100. */
int lm_sysinfo_cpu_percent(const struct lm_cpu_times *prev, const struct lm_cpu_times *cur);
/* /proc/meminfo; false when MemTotal / MemAvailable are missing (MemAvailable falls back to Free+Buffers+Cached). */
bool lm_sysinfo_parse_meminfo(const char *text, long *total, long *avail, long *swap_total, long *swap_free);
/* Finds the CPU temperature under `root` (""/"/tmp/x"): hwmon coretemp/k10temp/... (the "Package"/"Tctl"/"Tdie" input, else
 * the hottest one), else a thermal zone of a CPU type, else any thermal zone. False when there is none. */
bool lm_sysinfo_read_temp(const char *root, int *celsius, char *label, size_t label_n);

/* ---- the live sampler ------------------------------------------------------------------------------------------ */

struct lm_loop;
struct lm_sysinfo;
struct lm_sysinfo *lm_sysinfo_create(struct lm_loop *loop, void (*changed)(void *), void *data);
void lm_sysinfo_destroy(struct lm_sysinfo *s);
/* Sample every second while `active` (the first sample is taken at once, the load needs a second one). */
void lm_sysinfo_set_active(struct lm_sysinfo *s, bool active);
const struct lm_sysinfo_state *lm_sysinfo_state(const struct lm_sysinfo *s);

#endif
