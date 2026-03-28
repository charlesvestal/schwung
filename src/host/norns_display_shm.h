#ifndef NORNS_DISPLAY_SHM_H
#define NORNS_DISPLAY_SHM_H

#include <stdint.h>

#define NORNS_DISPLAY_MAGIC   "NR4SHM1"
#define NORNS_DISPLAY_FORMAT  "gray4"
#define NORNS_FRAME_WIDTH     128
#define NORNS_FRAME_HEIGHT    64
#define NORNS_FRAME_SIZE      4096  /* 128 * 64 / 2 (4-bit packed) */
#define NORNS_STALE_MS        1500

typedef struct __attribute__((packed)) {
    char magic[8];
    char format[16];
    uint64_t last_update_ms;
    uint32_t version;
    uint32_t header_size;
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_frame;
    uint32_t frame_counter;
    uint8_t active;
    uint8_t reserved[7];
    uint8_t frame[NORNS_FRAME_SIZE];
} norns_display_shm_t;

#endif /* NORNS_DISPLAY_SHM_H */
