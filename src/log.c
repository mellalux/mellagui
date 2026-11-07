#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static enum lm_log_level g_level = LM_LOG_INFO;
static int g_file_fd = -1;
static int g_color = -1; /* -1 = not probed yet */

int lm_log_level_from_string(const char *s)
{
    static const struct {
        const char *name;
        int level;
    } names[] = {
        { "error", LM_LOG_ERROR }, { "warn", LM_LOG_WARN },
        { "warning", LM_LOG_WARN }, { "info", LM_LOG_INFO },
        { "debug", LM_LOG_DEBUG }, { "trace", LM_LOG_TRACE },
        { "0", LM_LOG_ERROR },     { "1", LM_LOG_WARN },
        { "2", LM_LOG_INFO },      { "3", LM_LOG_DEBUG },
        { "4", LM_LOG_TRACE },
    };
    if (!s)
        return -1;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strcasecmp(s, names[i].name) == 0)
            return names[i].level;
    return -1;
}

void lm_log_set_level(enum lm_log_level level)
{
    if (level < LM_LOG_ERROR)
        level = LM_LOG_ERROR;
    if (level > LM_LOG_TRACE)
        level = LM_LOG_TRACE;
    g_level = level;
}

enum lm_log_level lm_log_get_level(void)
{
    return g_level;
}

int lm_log_set_file(const char *path)
{
    int fd = -1;
    if (path) {
        fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
        if (fd < 0)
            return -1;
    }
    if (g_file_fd >= 0)
        close(g_file_fd);
    g_file_fd = fd;
    return 0;
}

void lm_log_close(void)
{
    if (g_file_fd >= 0)
        close(g_file_fd);
    g_file_fd = -1;
}

static void write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return; /* logging must never fail loudly */
        }
        buf += n;
        len -= (size_t)n;
    }
}

void lm_log_vwrite(enum lm_log_level level, const char *file, int line,
                   const char *fmt, va_list ap)
{
    static const char *const names[] = { "ERROR", "WARN ", "INFO ", "DEBUG", "TRACE" };
    static const char *const colors[] = { "\033[1;31m", "\033[1;33m", "\033[0;32m",
                                          "\033[0;36m", "\033[0;90m" };
    if (level < LM_LOG_ERROR || level > LM_LOG_TRACE)
        level = LM_LOG_ERROR;
    if (level > g_level)
        return;

    int saved_errno = errno;
    struct timespec ts;
    struct tm tm;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);

    const char *base = file ? strrchr(file, '/') : NULL;
    base = base ? base + 1 : (file ? file : "?");

    char msg[1536];
    int n = vsnprintf(msg, sizeof(msg), fmt, ap);
    if (n < 0)
        n = 0;
    if ((size_t)n >= sizeof(msg))
        n = (int)sizeof(msg) - 1;

    if (g_color < 0)
        g_color = isatty(STDERR_FILENO) ? 1 : 0;

    char out[2048];
    int len = snprintf(out, sizeof(out), "%02d:%02d:%02d.%03ld %s %s:%d: %.*s\n",
                       tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000L,
                       names[level], base, line, n, msg);
    if (len < 0)
        goto out;
    if ((size_t)len >= sizeof(out)) {
        len = (int)sizeof(out) - 1;
        out[len - 1] = '\n';
    }

    if (g_file_fd >= 0)
        write_all(g_file_fd, out, (size_t)len);
    if (g_color) {
        char cbuf[2048 + 32];
        int clen = snprintf(cbuf, sizeof(cbuf), "%s%.*s\033[0m", colors[level],
                            len - 1, out);
        if (clen > 0 && (size_t)clen < sizeof(cbuf) - 1) {
            cbuf[clen++] = '\n';
            write_all(STDERR_FILENO, cbuf, (size_t)clen);
        } else {
            write_all(STDERR_FILENO, out, (size_t)len);
        }
    } else {
        write_all(STDERR_FILENO, out, (size_t)len);
    }
out:
    errno = saved_errno;
}

void lm_log_write(enum lm_log_level level, const char *file, int line,
                  const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    lm_log_vwrite(level, file, line, fmt, ap);
    va_end(ap);
}
