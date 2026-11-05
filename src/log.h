#ifndef LUMEN_LOG_H
#define LUMEN_LOG_H

/*
 * Leveled logging to stderr and, optionally, to a file.
 * Not async-signal-safe. Owner: log.c.
 */

#include <stdarg.h>

enum lm_log_level {
    LM_LOG_ERROR = 0,
    LM_LOG_WARN,
    LM_LOG_INFO,
    LM_LOG_DEBUG,
    LM_LOG_TRACE,
};

/* "error|warn|info|debug|trace" (case-insensitive) or "0".."4"; -1 if unknown. */
int lm_log_level_from_string(const char *s);

void lm_log_set_level(enum lm_log_level level);
enum lm_log_level lm_log_get_level(void);

/* Additionally append all log lines to `path`. Returns 0 or -1 (errno set). */
int lm_log_set_file(const char *path);
void lm_log_close(void);

void lm_log_write(enum lm_log_level level, const char *file, int line,
                  const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void lm_log_vwrite(enum lm_log_level level, const char *file, int line,
                   const char *fmt, va_list ap);

#define LM_LOG(level, ...) \
    do { \
        if ((level) <= lm_log_get_level()) \
            lm_log_write((level), __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

#define lm_error(...) LM_LOG(LM_LOG_ERROR, __VA_ARGS__)
#define lm_warn(...)  LM_LOG(LM_LOG_WARN, __VA_ARGS__)
#define lm_info(...)  LM_LOG(LM_LOG_INFO, __VA_ARGS__)
#define lm_debug(...) LM_LOG(LM_LOG_DEBUG, __VA_ARGS__)
#define lm_trace(...) LM_LOG(LM_LOG_TRACE, __VA_ARGS__)

#endif
