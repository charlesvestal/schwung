#include "unified_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>

static FILE *log_file = NULL;
static int log_crash_fd = -1;  /* Async-signal-safe FD for crash logging */
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
            /* Keep a raw FD for async-signal-safe crash logging */
            log_crash_fd = fileno(log_file);
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
    /* Non-blocking: if mutex is held (e.g. by a logging thread doing fflush),
     * just return the cached value to avoid blocking the audio thread. */
    if (pthread_mutex_trylock(&log_mutex) == 0) {
        /* Periodically recheck flag file */
        if (++check_counter >= CHECK_INTERVAL) {
            check_counter = 0;
            log_enabled_cache = (access(UNIFIED_LOG_FLAG, F_OK) == 0) ? 1 : 0;
        }
        pthread_mutex_unlock(&log_mutex);
    }
    return log_enabled_cache;
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
    /* Non-blocking: skip log message if mutex is held (avoids blocking audio thread).
     * Log messages are best-effort — dropping one is better than an audio click. */
    if (pthread_mutex_trylock(&log_mutex) != 0) {
        return;  /* Mutex held by another thread, drop this message */
    }

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

/* Async-signal-safe integer-to-string helper */
static int crash_itoa(int val, char *buf, int buflen) {
    if (buflen < 2) return 0;
    if (val < 0) {
        buf[0] = '-';
        int n = crash_itoa(-val, buf + 1, buflen - 1);
        return n + 1;
    }
    /* Write digits in reverse, then flip */
    char tmp[16];
    int len = 0;
    if (val == 0) { tmp[len++] = '0'; }
    while (val > 0 && len < (int)sizeof(tmp)) {
        tmp[len++] = '0' + (val % 10);
        val /= 10;
    }
    if (len >= buflen) len = buflen - 1;
    for (int i = 0; i < len; i++) buf[i] = tmp[len - 1 - i];
    return len;
}

void unified_log_crash(const char *msg) {
    int fd = log_crash_fd;
    if (fd < 0) {
        /* Try opening directly as last resort */
        fd = open(UNIFIED_LOG_PATH, O_WRONLY | O_APPEND | O_CREAT, 0666);
        if (fd < 0) return;
    }

    /* Build message using only async-signal-safe calls */
    char buf[256];
    int pos = 0;

    /* Timestamp: seconds since epoch (async-signal-safe via clock_gettime) */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    pos += crash_itoa((int)(ts.tv_sec % 100000), buf + pos, sizeof(buf) - pos);
    buf[pos++] = '.';
    pos += crash_itoa((int)(ts.tv_nsec / 1000000), buf + pos, sizeof(buf) - pos);

    /* Header */
    const char hdr[] = " [CRASH] [shim] ";
    int hdr_len = sizeof(hdr) - 1;
    if (pos + hdr_len < (int)sizeof(buf)) {
        for (int i = 0; i < hdr_len; i++) buf[pos++] = hdr[i];
    }

    /* Message */
    if (msg) {
        int i = 0;
        while (msg[i] && pos < (int)sizeof(buf) - 2) buf[pos++] = msg[i++];
    }
    buf[pos++] = '\n';

    write(fd, buf, pos);

    /* If we opened a new fd, close it */
    if (fd != log_crash_fd) close(fd);
}
