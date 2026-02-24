/* shadow_fd_trace.h - MIDI/SPI file descriptor tracing for debug
 * Extracted from move_anything_shim.c for maintainability. */

#ifndef SHADOW_FD_TRACE_H
#define SHADOW_FD_TRACE_H

#include <stddef.h>

/* Check if MIDI FD tracing is enabled (flag file on disk). */
int trace_midi_fd_enabled(void);

/* Open the MIDI trace log file (lazy, opens once). */
void midi_fd_trace_log_open(void);

/* Check if SPI I/O tracing is enabled (flag file on disk). */
int trace_spi_io_enabled(void);

/* Open the SPI I/O log file (lazy, opens once). */
void spi_io_log_open(void);

/* Check if a path refers to a MIDI device. */
int path_matches_midi(const char *path);

/* Check if a path refers to an SPI device. */
int path_matches_spi(const char *path);

/* Track an opened file descriptor. */
void track_fd(int fd, const char *path);

/* Stop tracking a file descriptor. */
void untrack_fd(int fd);

/* Look up the path associated with a tracked FD. */
const char *tracked_path_for_fd(int fd);

/* Log raw bytes from a read/write on a MIDI or SPI fd. */
void log_fd_bytes(const char *tag, int fd, const char *path,
                  const unsigned char *buf, size_t len);

/* Log a simple event (OPEN, CLOSE, etc.) to the MIDI trace log. */
void fd_trace_log_midi(const char *tag, int fd, const char *path);

/* Log a simple event (OPEN, CLOSE, etc.) to the SPI I/O log. */
void fd_trace_log_spi(const char *tag, int fd, const char *path);

#endif /* SHADOW_FD_TRACE_H */
