/*
 * generic-display-test.c - Test producer for generic display SHM
 *
 * Writes a moving gradient pattern at the specified resolution.
 * Usage: generic-display-test [width] [height] [fps]
 *        Defaults: 480 320 30
 *
 * Also monitors touch_counter and prints touch events to stdout.
 */

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "generic_display_shm.h"

static volatile int running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char *argv[]) {
    uint32_t width  = argc > 1 ? (uint32_t)atoi(argv[1]) : 480;
    uint32_t height = argc > 2 ? (uint32_t)atoi(argv[2]) : 320;
    int fps         = argc > 3 ? atoi(argv[3]) : 30;

    uint32_t bpp = 3;  /* RGB888 */
    uint32_t frame_size = width * height * bpp;
    size_t total_size = GENERIC_DISPLAY_HEADER_SIZE + frame_size;

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* Create SHM */
    int fd = open(GENERIC_DISPLAY_SHM_PATH, O_CREAT | O_RDWR, 0666);
    if (fd < 0) { perror("open shm"); return 1; }
    if (ftruncate(fd, total_size) < 0) { perror("ftruncate"); return 1; }

    generic_display_shm_t *shm = mmap(NULL, total_size,
                                       PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (shm == MAP_FAILED) { perror("mmap"); return 1; }

    /* Init header */
    memcpy(shm->magic, GENERIC_DISPLAY_MAGIC, 8);
    strncpy(shm->format, GENERIC_FMT_RGB888, 16);
    shm->version = 1;
    shm->header_size = GENERIC_DISPLAY_HEADER_SIZE;
    shm->width = width;
    shm->height = height;
    shm->bytes_per_pixel = bpp;
    shm->bytes_per_frame = frame_size;
    shm->frame_counter = 0;
    shm->touch_counter = 0;
    shm->active = 1;

    uint8_t *frame = ((uint8_t *)shm) + GENERIC_DISPLAY_HEADER_SIZE;
    uint32_t last_touch = 0;
    int offset = 0;
    long long interval_ms = 1000 / fps;

    printf("Streaming %ux%u RGB888 @ %d fps (%zu bytes/frame)\n",
           width, height, fps, (size_t)frame_size);
    printf("SHM: %s\n", GENERIC_DISPLAY_SHM_PATH);

    while (running) {
        long long start = now_ms();

        /* Draw a moving gradient */
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint32_t idx = (y * width + x) * 3;
                frame[idx + 0] = (x + offset) & 0xFF;       /* R */
                frame[idx + 1] = (y + offset / 2) & 0xFF;   /* G */
                frame[idx + 2] = (x + y + offset) & 0xFF;   /* B */
            }
        }
        __sync_synchronize();
        shm->frame_counter++;
        shm->last_update_ms = (uint64_t)now_ms();
        offset += 2;

        /* Check for touch events */
        if (shm->touch_counter != last_touch) {
            printf("Touch: x=%u y=%u state=%u\n",
                   shm->touch_x, shm->touch_y, shm->touch_state);
            last_touch = shm->touch_counter;
        }

        /* Sleep for remainder of frame interval */
        long long elapsed = now_ms() - start;
        if (elapsed < interval_ms)
            usleep((unsigned int)((interval_ms - elapsed) * 1000));
    }

    shm->active = 0;
    munmap(shm, total_size);
    close(fd);
    unlink(GENERIC_DISPLAY_SHM_PATH);
    printf("Cleaned up.\n");
    return 0;
}
