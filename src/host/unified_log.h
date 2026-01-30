#ifndef UNIFIED_LOG_H
#define UNIFIED_LOG_H

#include <stdarg.h>

/* Log levels */
#define LOG_LEVEL_ERROR 0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3

/* Default log file location */
#define UNIFIED_LOG_PATH "/data/UserData/move-anything/debug.log"
#define UNIFIED_LOG_FLAG "/data/UserData/move-anything/debug_log_on"

/* Initialize/shutdown logging system */
void unified_log_init(void);
void unified_log_shutdown(void);

/* Check if logging is enabled (cached, checks flag file periodically) */
int unified_log_enabled(void);

/* Core logging functions */
void unified_log(const char *source, int level, const char *fmt, ...);
void unified_log_v(const char *source, int level, const char *fmt, va_list args);

/* Convenience macros */
#define LOG_ERROR(src, ...) unified_log(src, LOG_LEVEL_ERROR, __VA_ARGS__)
#define LOG_WARN(src, ...)  unified_log(src, LOG_LEVEL_WARN, __VA_ARGS__)
#define LOG_INFO(src, ...)  unified_log(src, LOG_LEVEL_INFO, __VA_ARGS__)
#define LOG_DEBUG(src, ...) unified_log(src, LOG_LEVEL_DEBUG, __VA_ARGS__)

#endif /* UNIFIED_LOG_H */
