#include "unified_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>

static FILE *log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static int log_enabled_cache = 0;
static int check_counter = 0;
#define CHECK_INTERVAL 100  /* Check flag file every N calls */

void unified_log_init(void) {
    pthread_mutex_lock(&log_mutex);
    if (!log_file) {
        log_file = fopen(UNIFIED_LOG_PATH, "a");
        if (log_file) {
            /* Write startup marker */
            time_t now = time(NULL);
            fprintf(log_file, "\n=== Log started: %s", ctime(&now));
            fflush(log_file);
        }
    }
    /* Initial flag check */
    log_enabled_cache = (access(UNIFIED_LOG_FLAG, F_OK) == 0) ? 1 : 0;
    pthread_mutex_unlock(&log_mutex);
}

void unified_log_shutdown(void) {
    pthread_mutex_lock(&log_mutex);
    if (log_file) {
        time_t now = time(NULL);
        fprintf(log_file, "=== Log ended: %s\n", ctime(&now));
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_mutex);
}

int unified_log_enabled(void) {
    pthread_mutex_lock(&log_mutex);
    /* Periodically recheck flag file */
    if (++check_counter >= CHECK_INTERVAL) {
        check_counter = 0;
        log_enabled_cache = (access(UNIFIED_LOG_FLAG, F_OK) == 0) ? 1 : 0;
    }
    int enabled = log_enabled_cache;
    pthread_mutex_unlock(&log_mutex);
    return enabled;
}

static const char *level_str(int level) {
    switch (level) {
        case LOG_LEVEL_ERROR: return "ERROR";
        case LOG_LEVEL_WARN:  return "WARN ";
        case LOG_LEVEL_INFO:  return "INFO ";
        case LOG_LEVEL_DEBUG: return "DEBUG";
        default: return "?????";
    }
}

void unified_log_v(const char *source, int level, const char *fmt, va_list args) {
    /* Single lock acquisition -- check enabled + write in one critical section */
    pthread_mutex_lock(&log_mutex);

    /* Periodically recheck flag file */
    if (++check_counter >= CHECK_INTERVAL) {
        check_counter = 0;
        log_enabled_cache = (access(UNIFIED_LOG_FLAG, F_OK) == 0) ? 1 : 0;
    }
    if (!log_enabled_cache) {
        pthread_mutex_unlock(&log_mutex);
        return;
    }

    if (!log_file) {
        log_file = fopen(UNIFIED_LOG_PATH, "a");
    }
    if (log_file) {
        /* Timestamp with milliseconds */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        struct tm tm_buf;
        struct tm *tm_info = localtime_r(&tv.tv_sec, &tm_buf);

        fprintf(log_file, "%02d:%02d:%02d.%03d [%s] [%s] ",
                tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec,
                (int)(tv.tv_usec / 1000),
                level_str(level),
                source ? source : "???");

        vfprintf(log_file, fmt, args);
        fprintf(log_file, "\n");
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mutex);
}

void unified_log(const char *source, int level, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    unified_log_v(source, level, fmt, args);
    va_end(args);
}
