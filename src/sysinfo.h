/*
 * sysinfo.h - CPU load, memory and temperature for the system-monitor gadget. Plain file reads (/proc/stat,
 * /proc/meminfo, /sys/class/hwmon, /sys/class/thermal): small and fast, so they are done in the event loop without child
 * processes. Env MELLAGUI_SYS_ROOT prefixes every path (a fake /proc and /sys tree for the tests).
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

/* The graphics chip the screen is on (read from /sys/class/drm, pci.ids and, for NVIDIA's own driver, `nvidia-smi`). -1 / 0 = not known. */
struct lm_gpu {
    bool present;
    char name[80];                      /* "Intel 3rd Gen Core processor Graphics Controller", "AMD Radeon RX 6700 XT" */
    char driver[24];                    /* "i915", "amdgpu", "nvidia", "nouveau" */
    int busy;                           /* per cent, -1 unknown */
    bool busy_estimated;                /* Intel: how long the chip was out of its sleep state (RC6) in the last second: no real counter without root */
    int freq_mhz, freq_max_mhz;         /* 0 unknown */
    int temp_c;                         /* -1 unknown */
    long vram_used_kb, vram_total_kb;   /* 0 = unknown (an integrated chip shares the main memory) */
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
    struct lm_gpu gpu;
    double load1;                       /* 1-minute load average */
    /* the device (read once when the sampler starts; uptime on every sample); an empty string = unknown */
    char maker[48];                     /* manufacturer: DMI sys_vendor / board_vendor, "devicetree" boards: first word of the model */
    char model[64];                     /* product: DMI product_name / product_version / board_name, else /proc/device-tree/model */
    char cpu_model[64];                 /* "Intel Core i5-8250U @ 1.60GHz" */
    char os_name[48];                   /* PRETTY_NAME of /etc/os-release */
    char kernel[40];                    /* /proc/sys/kernel/osrelease */
    char host[48];
    long uptime_s;                      /* -1 = unknown */
};

/* ---- pure helpers (unit-tested) -------------------------------------------------------------------------------- */

/* /proc/stat: the "cpu " line into `total`, the "cpuN" lines into cores[]; returns the number of cores. */
int lm_sysinfo_parse_stat(const char *text, struct lm_cpu_times *total, struct lm_cpu_times *cores, int max);
/* Load between two samples, percent 0..100. */
int lm_sysinfo_cpu_percent(const struct lm_cpu_times *prev, const struct lm_cpu_times *cur);
/* /proc/meminfo; false when MemTotal / MemAvailable are missing (MemAvailable falls back to Free+Buffers+Cached). */
bool lm_sysinfo_parse_meminfo(const char *text, long *total, long *avail, long *swap_total, long *swap_free);
/* /proc/cpuinfo: the CPU model ("model name" / "Model" / "Hardware" / "cpu model"), without (R), (TM), "CPU" and doubled
 * blanks. False when there is none. */
bool lm_sysinfo_parse_cpu_model(const char *text, char *out, size_t n);
/* /etc/os-release: PRETTY_NAME (quotes removed), else NAME. False when there is none. */
bool lm_sysinfo_parse_os_name(const char *text, char *out, size_t n);
/* /proc/uptime in whole seconds, -1 when unreadable. */
long lm_sysinfo_parse_uptime(const char *text);
/* "3d 4h", "5h 12m", "7m": the two largest units. */
void lm_sysinfo_format_uptime(long seconds, char *out, size_t n);
/* Fills maker / model of `st` from the DMI tables (or the device tree) under `root`; generic placeholders such as
 * "To be filled by O.E.M." are skipped, a vendor name that the model repeats is not shown twice. */
void lm_sysinfo_read_device(const char *root, struct lm_sysinfo_state *st);

/* Finds the CPU temperature under `root` (""/"/tmp/x"): hwmon coretemp/k10temp/... (the "Package"/"Tctl"/"Tdie" input, else
 * the hottest one), else a thermal zone of a CPU type, else any thermal zone. False when there is none. */
bool lm_sysinfo_read_temp(const char *root, int *celsius, char *label, size_t label_n);

/* pci.ids text: "Vendor Device" for the ids ("Intel 3rd Gen Core processor Graphics Controller"; the vendor is shortened, a [marketing name] in
 * brackets wins over the chip name). False when the ids are not in the text. */
bool lm_sysinfo_pci_name(const char *pci_ids, unsigned vendor, unsigned device, char *out, size_t n);
/* amdgpu's pp_dpm_sclk ("0: 300Mhz\n1: 1200Mhz *\n..."): the starred line and the highest one, in MHz. */
bool lm_sysinfo_parse_dpm(const char *text, int *cur, int *max);
/* `nvidia-smi --query-gpu=name,utilization.gpu,memory.used,memory.total,temperature.gpu,clocks.gr,clocks.max.gr --format=csv,noheader,nounits`. */
bool lm_sysinfo_parse_nvidia_smi(const char *text, struct lm_gpu *gpu);

/* ---- the live sampler ------------------------------------------------------------------------------------------ */

struct lm_loop;
struct lm_sysinfo;
struct lm_sysinfo *lm_sysinfo_create(struct lm_loop *loop, void (*changed)(void *), void *data);
void lm_sysinfo_destroy(struct lm_sysinfo *s);
/* Sample every second while `active` (the first sample is taken at once, the load needs a second one). */
void lm_sysinfo_set_active(struct lm_sysinfo *s, bool active);
const struct lm_sysinfo_state *lm_sysinfo_state(const struct lm_sysinfo *s);

#endif
