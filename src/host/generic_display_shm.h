#ifndef GENERIC_DISPLAY_SHM_H
#define GENERIC_DISPLAY_SHM_H

#include <stdint.h>

#define GENERIC_DISPLAY_MAGIC      "SCHWDSP1"
#define GENERIC_DISPLAY_SHM_PATH   "/dev/shm/schwung-display-generic"

/* Pixel formats (stored in format field) */
#define GENERIC_FMT_RGB888         "rgb888"      /* 3 bytes/pixel */
#define GENERIC_FMT_RGBA8888       "rgba8888"     /* 4 bytes/pixel */
#define GENERIC_FMT_GRAY8          "gray8"        /* 1 byte/pixel  */

/* Touch states */
#define GENERIC_TOUCH_NONE         0
#define GENERIC_TOUCH_DOWN         1
#define GENERIC_TOUCH_MOVE         2
#define GENERIC_TOUCH_UP           3

/* Staleness threshold */
#define GENERIC_STALE_MS           1500

/* Fixed header size (frame data follows immediately after) */
#define GENERIC_DISPLAY_HEADER_SIZE 96

typedef struct __attribute__((packed)) {
    /* Identity (offset 0) */
    char     magic[8];            /* "SCHWDSP1" */
    char     format[16];          /* "rgb888", "rgba8888", "gray8" */

    /* Timing (offset 24) */
    uint64_t last_update_ms;      /* CLOCK_MONOTONIC ms, set by producer */

    /* Versioning (offset 32) */
    uint32_t version;             /* 1 */
    uint32_t header_size;         /* GENERIC_DISPLAY_HEADER_SIZE */

    /* Dimensions (offset 40) */
    uint32_t width;               /* pixels */
    uint32_t height;              /* pixels */
    uint32_t bytes_per_pixel;     /* 1, 3, or 4 */
    uint32_t bytes_per_frame;     /* width * height * bytes_per_pixel */

    /* Frame sequencing (offset 56) */
    uint32_t frame_counter;       /* bumped after each frame write */
    uint8_t  active;              /* 1 = live, 0 = dormant */
    uint8_t  reserved1[3];

    /* Touch back-channel (offset 64) — display_server writes, app reads */
    uint32_t touch_x;            /* pixel coordinate */
    uint32_t touch_y;            /* pixel coordinate */
    uint32_t touch_state;        /* GENERIC_TOUCH_* */
    uint32_t touch_counter;      /* bumped on each new touch event */
    uint32_t touch_id;           /* multi-touch identifier (0 for single) */
    uint32_t touch_pressure;     /* 0-65535 (0 if not available) */

    uint8_t  reserved2[8];       /* pad to 96 bytes */

    /* Frame data follows: uint8_t frame[bytes_per_frame] */
} generic_display_shm_t;

/* Compile-time size check */
typedef char generic_header_size_check[
    (sizeof(generic_display_shm_t) == GENERIC_DISPLAY_HEADER_SIZE) ? 1 : -1];

#endif /* GENERIC_DISPLAY_SHM_H */
