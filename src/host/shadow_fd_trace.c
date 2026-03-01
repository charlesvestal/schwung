/* shadow_fd_trace.c - MIDI/SPI file descriptor tracing for debug
 * Extracted from move_anything_shim.c for maintainability. */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "shadow_fd_trace.h"

/* ============================================================================
 * Internal types and state
 * ============================================================================ */

#define MAX_TRACKED_FDS 32

typedef struct {
    int fd;
    char path[128];
} tracked_fd_t;

static tracked_fd_t tracked_fds[MAX_TRACKED_FDS];
static FILE *midi_fd_trace_log = NULL;
static FILE *spi_io_log = NULL;

/* ============================================================================
 * Utility
 * ============================================================================ */

static void str_to_lower(char *dst, size_t dst_size, const char *src)
{
    size_t i = 0;
    if (!dst_size) return;
    for (; i + 1 < dst_size && src[i]; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
    }
    dst[i] = '\0';
}

/* ============================================================================
 * Flag file checks (cached)
 * ============================================================================ */

int trace_midi_fd_enabled(void)
{
    static int enabled = -1;
    static int check_counter = 0;
    if (check_counter++ % 200 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/midi_fd_trace_on", F_OK) == 0);
    }
    return enabled;
}

void midi_fd_trace_log_open(void)
{
    if (!midi_fd_trace_log) {
        midi_fd_trace_log = fopen("/data/UserData/move-anything/midi_fd_trace.log", "a");
    }
}

int trace_spi_io_enabled(void)
{
    static int enabled = -1;
    static int check_counter = 0;
    if (check_counter++ % 200 == 0 || enabled < 0) {
        enabled = (access("/data/UserData/move-anything/spi_io_on", F_OK) == 0);
    }
    return enabled;
}

void spi_io_log_open(void)
{
    if (!spi_io_log) {
        spi_io_log = fopen("/data/UserData/move-anything/spi_io.log", "a");
    }
}

/* ============================================================================
 * Path matching
 * ============================================================================ */

int path_matches_midi(const char *path)
{
    if (!path || !*path) return 0;
    char lower[256];
    str_to_lower(lower, sizeof(lower), path);
    return strstr(lower, "midi") || strstr(lower, "snd") ||
           strstr(lower, "seq") || strstr(lower, "usb");
}

int path_matches_spi(const char *path)
{
    if (!path || !*path) return 0;
    char lower[256];
    str_to_lower(lower, sizeof(lower), path);
    return strstr(lower, "ablspi") || strstr(lower, "spidev") ||
           strstr(lower, "/spi");
}

/* ============================================================================
 * FD tracking
 * ============================================================================ */

void track_fd(int fd, const char *path)
{
    if (fd < 0 || !path) return;
    for (int i = 0; i < MAX_TRACKED_FDS; i++) {
        if (tracked_fds[i].fd == 0) {
            tracked_fds[i].fd = fd;
            strncpy(tracked_fds[i].path, path, sizeof(tracked_fds[i].path) - 1);
            tracked_fds[i].path[sizeof(tracked_fds[i].path) - 1] = '\0';
            return;
        }
    }
}

void untrack_fd(int fd)
{
    for (int i = 0; i < MAX_TRACKED_FDS; i++) {
        if (tracked_fds[i].fd == fd) {
            tracked_fds[i].fd = 0;
            tracked_fds[i].path[0] = '\0';
            return;
        }
    }
}

const char *tracked_path_for_fd(int fd)
{
    for (int i = 0; i < MAX_TRACKED_FDS; i++) {
        if (tracked_fds[i].fd == fd) {
            return tracked_fds[i].path;
        }
    }
    return NULL;
}

/* ============================================================================
 * Byte logging
 * ============================================================================ */

void log_fd_bytes(const char *tag, int fd, const char *path,
                  const unsigned char *buf, size_t len)
{
    size_t max = len > 64 ? 64 : len;
    if (path_matches_midi(path)) {
        if (!trace_midi_fd_enabled()) return;
        midi_fd_trace_log_open();
        if (!midi_fd_trace_log) return;
        fprintf(midi_fd_trace_log, "%s fd=%d path=%s len=%zu bytes:", tag, fd, path, len);
        for (size_t i = 0; i < max; i++) {
            fprintf(midi_fd_trace_log, " %02x", buf[i]);
        }
        if (len > max) fprintf(midi_fd_trace_log, " ...");
        fprintf(midi_fd_trace_log, "\n");
        fflush(midi_fd_trace_log);
    }
    if (path_matches_spi(path)) {
        if (!trace_spi_io_enabled()) return;
        spi_io_log_open();
        if (!spi_io_log) return;
        fprintf(spi_io_log, "%s fd=%d path=%s len=%zu bytes:", tag, fd, path, len);
        for (size_t i = 0; i < max; i++) {
            fprintf(spi_io_log, " %02x", buf[i]);
        }
        if (len > max) fprintf(spi_io_log, " ...");
        fprintf(spi_io_log, "\n");
        fflush(spi_io_log);
    }
}

/* ============================================================================
 * Simple event logging (OPEN, CLOSE, etc.)
 * ============================================================================ */

void fd_trace_log_midi(const char *tag, int fd, const char *path)
{
    midi_fd_trace_log_open();
    if (!midi_fd_trace_log) return;
    fprintf(midi_fd_trace_log, "%s fd=%d path=%s\n", tag, fd, path);
    fflush(midi_fd_trace_log);
}

void fd_trace_log_spi(const char *tag, int fd, const char *path)
{
    spi_io_log_open();
    if (!spi_io_log) return;
    fprintf(spi_io_log, "%s fd=%d path=%s\n", tag, fd, path);
    fflush(spi_io_log);
}
