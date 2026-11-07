#include "sysinfo.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "loop.h"

/* ---- reading files ---------------------------------------------------------------------------------------------- */

static const char *root_prefix(void)
{
    const char *r = getenv("MELLAGUI_SYS_ROOT");
    return r ? r : "";
}

static bool read_file(const char *root, const char *path, char *buf, size_t n)
{
    char full[512];
    snprintf(full, sizeof(full), "%s%s", root, path);
    FILE *f = fopen(full, "r");
    if (!f)
        return false;
    size_t len = fread(buf, 1, n - 1, f);
    buf[len] = 0;
    fclose(f);
    return true;
}

/* ---- parsers ----------------------------------------------------------------------------------------------------- */

static bool parse_cpu_line(const char *p, struct lm_cpu_times *out)
{
    unsigned long long v[8] = { 0 };
    int n = sscanf(p, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (n < 4)
        return false;
    unsigned long long idle = v[3] + (n > 4 ? v[4] : 0);                /* idle + iowait */
    unsigned long long total = 0;
    for (int i = 0; i < n; i++)
        total += v[i];
    out->total = total;
    out->busy = total > idle ? total - idle : 0;
    return true;
}

int lm_sysinfo_parse_stat(const char *text, struct lm_cpu_times *total, struct lm_cpu_times *cores, int max)
{
    int n = 0;
    memset(total, 0, sizeof(*total));
    const char *p = text;
    while (*p) {
        if (!strncmp(p, "cpu ", 4)) {
            parse_cpu_line(p + 4, total);
        } else if (!strncmp(p, "cpu", 3) && isdigit((unsigned char)p[3])) {
            const char *q = p + 3;
            while (isdigit((unsigned char)*q))
                q++;
            if (n < max && parse_cpu_line(q, &cores[n]))
                n++;
        }
        const char *eol = strchr(p, '\n');
        if (!eol)
            break;
        p = eol + 1;
    }
    return n;
}

int lm_sysinfo_cpu_percent(const struct lm_cpu_times *prev, const struct lm_cpu_times *cur)
{
    if (cur->total <= prev->total)
        return 0;
    unsigned long long dt = cur->total - prev->total, db = cur->busy > prev->busy ? cur->busy - prev->busy : 0;
    int pc = (int)((db * 100 + dt / 2) / dt);
    return pc < 0 ? 0 : pc > 100 ? 100 : pc;
}

static bool meminfo_value(const char *text, const char *key, long *out)
{
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, kl) && p[kl] == ':') {
            *out = atol(p + kl + 1);
            return true;
        }
    return false;
}

bool lm_sysinfo_parse_meminfo(const char *text, long *total, long *avail, long *swap_total, long *swap_free)
{
    long free_kb = 0, buffers = 0, cached = 0;
    *total = *avail = *swap_total = *swap_free = 0;
    if (!meminfo_value(text, "MemTotal", total))
        return false;
    if (!meminfo_value(text, "MemAvailable", avail)) {                  /* old kernels */
        meminfo_value(text, "MemFree", &free_kb);
        meminfo_value(text, "Buffers", &buffers);
        meminfo_value(text, "Cached", &cached);
        *avail = free_kb + buffers + cached;
    }
    meminfo_value(text, "SwapTotal", swap_total);
    meminfo_value(text, "SwapFree", swap_free);
    return *total > 0;
}

static bool name_is_cpu(const char *name)
{
    static const char *const cpu_names[] = { "coretemp", "k10temp", "zenpower", "cpu_thermal", "cpu-thermal", "x86_pkg_temp", "soc_thermal", NULL };
    for (int i = 0; cpu_names[i]; i++)
        if (!strcmp(name, cpu_names[i]))
            return true;
    return false;
}

static void trim_nl(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == ' '))
        s[--n] = 0;
}

bool lm_sysinfo_read_temp(const char *root, int *celsius, char *label, size_t label_n)
{
    char path[256], buf[128];
    /* 1. hwmon of a CPU driver: the package / die sensor, else the hottest input */
    for (int h = 0; h < 16; h++) {
        char hw[64], name[64];
        snprintf(hw, sizeof(hw), "/sys/class/hwmon/hwmon%d", h);
        snprintf(path, sizeof(path), "%s/name", hw);
        if (!read_file(root, path, name, sizeof(name)))
            continue;
        trim_nl(name);
        if (!name_is_cpu(name))
            continue;
        int best = -1000000, pkg = -1000000;
        char best_label[48] = "", pkg_label[48] = "";
        for (int t = 1; t <= 16; t++) {
            snprintf(path, sizeof(path), "%s/temp%d_input", hw, t);
            if (!read_file(root, path, buf, sizeof(buf)))
                continue;
            int v = atoi(buf);                                      /* millidegrees */
            char lab[48] = "";
            snprintf(path, sizeof(path), "%s/temp%d_label", hw, t);
            if (read_file(root, path, lab, sizeof(lab)))
                trim_nl(lab);
            if (v > best) {
                best = v;
                snprintf(best_label, sizeof(best_label), "%s", lab);
            }
            if (!strncmp(lab, "Package", 7) || !strcmp(lab, "Tctl") || !strcmp(lab, "Tdie")) {
                pkg = v;
                snprintf(pkg_label, sizeof(pkg_label), "%s", lab);
            }
        }
        if (best > -1000000) {
            int v = pkg > -1000000 ? pkg : best;
            *celsius = (v + 500) / 1000;
            snprintf(label, label_n, "%s %s", name, pkg > -1000000 ? pkg_label : best_label);
            trim_nl(label);
            return true;
        }
    }
    /* 2. a thermal zone: a CPU type first, then the first one that reads */
    int first = -1000000;
    char first_type[48] = "";
    for (int z = 0; z < 16; z++) {
        char type[48];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", z);
        if (!read_file(root, path, type, sizeof(type)))
            continue;
        trim_nl(type);
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
        if (!read_file(root, path, buf, sizeof(buf)))
            continue;
        int v = atoi(buf);
        if (name_is_cpu(type)) {
            *celsius = (v + 500) / 1000;
            snprintf(label, label_n, "%s", type);
            return true;
        }
        if (first == -1000000) {
            first = v;
            snprintf(first_type, sizeof(first_type), "%s", type);
        }
    }
    if (first > -1000000) {
        *celsius = (first + 500) / 1000;
        snprintf(label, label_n, "%s", first_type);
        return true;
    }
    return false;
}


/* ---- the device ---------------------------------------------------------------------------------------------------- */

static void collapse_blanks(char *s)
{
    char *w = s;
    bool blank = true;                                                  /* also strips the leading blanks */
    for (char *r = s; *r; r++) {
        if (*r == ' ' || *r == '\t') {
            if (!blank)
                *w++ = ' ';
            blank = true;
        } else {
            *w++ = *r;
            blank = false;
        }
    }
    if (w > s && w[-1] == ' ')
        w--;
    *w = 0;
}

static void remove_word(char *s, const char *word)
{
    size_t wl = strlen(word);
    for (char *p = strstr(s, word); p; p = strstr(p, word))
        memmove(p, p + wl, strlen(p + wl) + 1);
}

bool lm_sysinfo_parse_cpu_model(const char *text, char *out, size_t n)
{
    static const char *const keys[] = { "model name", "Model", "Hardware", "cpu model", "Processor" };
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        size_t kl = strlen(keys[k]);
        for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
            if (strncmp(p, keys[k], kl))
                continue;
            const char *q = p + kl;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q != ':')
                continue;
            q++;
            char tmp[160];
            size_t len = strcspn(q, "\n");
            if (len >= sizeof(tmp))
                len = sizeof(tmp) - 1;
            memcpy(tmp, q, len);
            tmp[len] = 0;
            remove_word(tmp, "(R)");
            remove_word(tmp, "(TM)");
            remove_word(tmp, " CPU");
            collapse_blanks(tmp);
            if (!tmp[0])
                continue;
            snprintf(out, n, "%s", tmp);
            return true;
        }
    }
    return false;
}

bool lm_sysinfo_parse_os_name(const char *text, char *out, size_t n)
{
    static const char *const keys[] = { "PRETTY_NAME", "NAME" };
    for (size_t k = 0; k < 2; k++) {
        size_t kl = strlen(keys[k]);
        for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
            if (strncmp(p, keys[k], kl) || p[kl] != '=')
                continue;
            const char *q = p + kl + 1;
            if (*q == '"' || *q == '\'')
                q++;
            size_t len = strcspn(q, "\n\"'");
            if (!len)
                continue;
            snprintf(out, n, "%.*s", (int)len, q);
            return true;
        }
    }
    return false;
}

long lm_sysinfo_parse_uptime(const char *text)
{
    char *end;
    double v = strtod(text, &end);
    return end == text || v < 0 ? -1 : (long)v;
}

void lm_sysinfo_format_uptime(long s, char *out, size_t n)
{
    if (s < 0)
        snprintf(out, n, "-");
    else if (s >= 86400)
        snprintf(out, n, "%ldd %ldh", s / 86400, s % 86400 / 3600);
    else if (s >= 3600)
        snprintf(out, n, "%ldh %ldm", s / 3600, s % 3600 / 60);
    else
        snprintf(out, n, "%ldm", s / 60);
}

static bool generic_name(const char *s)
{
    static const char *const bad[] = { "to be filled", "default string", "system manufacturer", "system product name", "system version",
                                       "not specified", "not applicable", "o.e.m.", "unknown", "none", "n/a", "type1", "0123456789", NULL };
    if (!s[0])
        return true;
    char low[96];
    size_t i = 0;
    for (; s[i] && i + 1 < sizeof(low); i++)
        low[i] = (char)tolower((unsigned char)s[i]);
    low[i] = 0;
    for (int b = 0; bad[b]; b++)
        if (strstr(low, bad[b]))
            return true;
    return false;
}

static void copy_str(char *dst, size_t n, const char *src)
{
    size_t len = strlen(src);
    if (len >= n)
        len = n - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
}

static bool read_line(const char *root, const char *path, char *out, size_t n)
{
    char buf[128];
    if (!read_file(root, path, buf, sizeof(buf)))
        return false;
    buf[strcspn(buf, "\n")] = 0;
    collapse_blanks(buf);
    copy_str(out, n, buf);
    return out[0] != 0;
}

static void tidy_vendor(char *s)
{
    remove_word(s, " Inc.");
    remove_word(s, " Inc");
    remove_word(s, " Corporation");
    remove_word(s, " Co., Ltd.");
    remove_word(s, " Ltd.");
    collapse_blanks(s);
}

void lm_sysinfo_read_device(const char *root, struct lm_sysinfo_state *st)
{
    char vendor[64] = "", name[96] = "", ver[96] = "", bvendor[64] = "", bname[96] = "";
    read_line(root, "/sys/class/dmi/id/sys_vendor", vendor, sizeof(vendor));
    read_line(root, "/sys/class/dmi/id/product_name", name, sizeof(name));
    read_line(root, "/sys/class/dmi/id/product_version", ver, sizeof(ver));
    read_line(root, "/sys/class/dmi/id/board_vendor", bvendor, sizeof(bvendor));
    read_line(root, "/sys/class/dmi/id/board_name", bname, sizeof(bname));
    st->maker[0] = st->model[0] = 0;
    if (generic_name(vendor))
        snprintf(vendor, sizeof(vendor), "%s", generic_name(bvendor) ? "" : bvendor);
    if (generic_name(name))
        snprintf(name, sizeof(name), "%s", generic_name(bname) ? "" : bname);
    /* Lenovo keeps the readable model ("ThinkPad T480") in product_version, the product name is a type number */
    if (!generic_name(ver) && strlen(ver) > 3 && (!name[0] || strcasestr(vendor, "lenovo")))
        snprintf(name, sizeof(name), "%s", ver);
    if (!vendor[0] && !name[0]) {
        char dt[96];
        if (read_file(root, "/proc/device-tree/model", dt, sizeof(dt)) && dt[0]) {   /* "Raspberry Pi 4 Model B Rev 1.4" */
            collapse_blanks(dt);
            copy_str(st->model, sizeof(st->model), dt);
            snprintf(st->maker, sizeof(st->maker), "%.*s", (int)strcspn(dt, " ,"), dt);
        }
        return;
    }
    tidy_vendor(vendor);
    copy_str(st->maker, sizeof(st->maker), vendor);
    if (vendor[0] && !strncasecmp(name, vendor, strlen(vendor)) && name[strlen(vendor)] == ' ')
        memmove(name, name + strlen(vendor) + 1, strlen(name + strlen(vendor) + 1) + 1);   /* "Dell Latitude" by "Dell" */
    copy_str(st->model, sizeof(st->model), name);
}

/* ---- the live sampler -------------------------------------------------------------------------------------------- */

struct lm_sysinfo {
    struct lm_loop *loop;
    void (*changed)(void *);
    void *data;
    struct lm_sysinfo_state st;
    struct lm_cpu_times prev_total, prev_core[LM_SYS_MAX_CORES];
    bool have_prev;
    bool active;
    struct lm_timer *timer;
};

static void sample(struct lm_sysinfo *s)
{
    const char *root = root_prefix();
    char buf[16384];
    struct lm_sysinfo_state *st = &s->st;
    struct lm_cpu_times total, cores[LM_SYS_MAX_CORES];
    if (read_file(root, "/proc/stat", buf, sizeof(buf))) {
        int n = lm_sysinfo_parse_stat(buf, &total, cores, LM_SYS_MAX_CORES);
        if (s->have_prev && n == st->ncores) {
            st->cpu = lm_sysinfo_cpu_percent(&s->prev_total, &total);
            for (int i = 0; i < n; i++)
                st->core[i] = lm_sysinfo_cpu_percent(&s->prev_core[i], &cores[i]);
            if (st->nhist == LM_SYS_HISTORY) {
                memmove(st->hist, st->hist + 1, sizeof(st->hist[0]) * (LM_SYS_HISTORY - 1));
                st->nhist--;
            }
            st->hist[st->nhist++] = st->cpu;
        }
        st->ncores = n;
        s->prev_total = total;
        memcpy(s->prev_core, cores, sizeof(cores[0]) * (size_t)n);
        s->have_prev = true;
    }
    if (read_file(root, "/proc/meminfo", buf, sizeof(buf)) &&
        lm_sysinfo_parse_meminfo(buf, &st->mem_total_kb, &st->mem_avail_kb, &st->swap_total_kb, &st->swap_free_kb)) {
        st->mem_pct = (int)((st->mem_total_kb - st->mem_avail_kb) * 100 / st->mem_total_kb);
        st->swap_pct = st->swap_total_kb > 0 ? (int)((st->swap_total_kb - st->swap_free_kb) * 100 / st->swap_total_kb) : 0;
    }
    st->temp_c = -1;
    st->temp_label[0] = 0;
    int c;
    if (lm_sysinfo_read_temp(root, &c, st->temp_label, sizeof(st->temp_label)))
        st->temp_c = c;
    if (read_file(root, "/proc/loadavg", buf, sizeof(buf)))
        st->load1 = atof(buf);
    st->uptime_s = read_file(root, "/proc/uptime", buf, sizeof(buf)) ? lm_sysinfo_parse_uptime(buf) : -1;
    st->ready = true;
    if (s->changed)
        s->changed(s->data);
}

/* The parts that do not change while the compositor runs. */
static void read_static(struct lm_sysinfo_state *st)
{
    const char *root = root_prefix();
    char buf[8192];
    lm_sysinfo_read_device(root, st);
    st->cpu_model[0] = st->os_name[0] = st->kernel[0] = st->host[0] = 0;
    if (read_file(root, "/proc/cpuinfo", buf, sizeof(buf)))
        lm_sysinfo_parse_cpu_model(buf, st->cpu_model, sizeof(st->cpu_model));
    if (read_file(root, "/etc/os-release", buf, sizeof(buf)))
        lm_sysinfo_parse_os_name(buf, st->os_name, sizeof(st->os_name));
    read_line(root, "/proc/sys/kernel/osrelease", st->kernel, sizeof(st->kernel));
    read_line(root, "/proc/sys/kernel/hostname", st->host, sizeof(st->host));
}

static void timer_cb(void *data)
{
    struct lm_sysinfo *s = data;
    if (s->active)
        sample(s);
}

struct lm_sysinfo *lm_sysinfo_create(struct lm_loop *loop, void (*changed)(void *), void *data)
{
    struct lm_sysinfo *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->loop = loop;
    s->changed = changed;
    s->data = data;
    s->st.temp_c = -1;
    s->st.uptime_s = -1;
    s->timer = lm_loop_add_timer(loop, timer_cb, s);
    return s;
}

void lm_sysinfo_destroy(struct lm_sysinfo *s)
{
    if (!s)
        return;
    if (s->timer)
        lm_timer_destroy(s->timer);
    free(s);
}

void lm_sysinfo_set_active(struct lm_sysinfo *s, bool active)
{
    if (!s || s->active == active)
        return;
    s->active = active;
    if (active) {
        s->have_prev = false;
        read_static(&s->st);
        sample(s);
        lm_timer_arm_ms(s->timer, 1000, 1000);
    } else {
        lm_timer_arm_ms(s->timer, 0, 0);
    }
}

const struct lm_sysinfo_state *lm_sysinfo_state(const struct lm_sysinfo *s)
{
    return &s->st;
}
