/*
 * shadow_test_stream.c — Phase 2 E2E test-bus stream publishers.
 * See shadow_test_stream.h for the public surface and rationale.
 */

#define _GNU_SOURCE

#include "shadow_test_stream.h"
#include "shadow_constants.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Per-channel SHM pointer. NULL = mmap failed; publisher no-ops in
 * that case (the test bus is dev-only opt-in; we never want a missing
 * SHM to crash the shim). */
static test_stream_shm_t *g_midi_out_shm = NULL;
static int                g_midi_out_fd  = -1;

static int open_stream_shm(const char *name, int *out_fd,
                           test_stream_shm_t **out_ptr) {
    int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        printf("Shadow: test-stream shm_open(%s) failed\n", name);
        return -1;
    }
    if (ftruncate(fd, sizeof(test_stream_shm_t)) < 0) {
        printf("Shadow: test-stream ftruncate(%s) failed\n", name);
        close(fd);
        return -1;
    }
    test_stream_shm_t *p = (test_stream_shm_t *)mmap(
        NULL, sizeof(test_stream_shm_t),
        PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        printf("Shadow: test-stream mmap(%s) failed\n", name);
        close(fd);
        return -1;
    }
    memset(p, 0, sizeof(test_stream_shm_t));
    *out_fd  = fd;
    *out_ptr = p;
    return 0;
}

void shadow_test_stream_init(void) {
    /* The MIDI_OUT channel is the only one in Phase 2; Phase 3 will add
     * more (MIDI_IN, log tail). Each channel = one call to
     * open_stream_shm. */
    open_stream_shm(SHM_TEST_STREAM_MIDI_OUT, &g_midi_out_fd, &g_midi_out_shm);
}

void shadow_test_stream_publish_midi_out(const uint8_t *hw_midi_out,
                                         uint32_t frame) {
    test_stream_shm_t *shm = g_midi_out_shm;
    if (!shm) return;
    if (!__atomic_load_n(&shm->enabled, __ATOMIC_RELAXED)) return;

    uint32_t seq = __atomic_load_n(&shm->write_seq, __ATOMIC_RELAXED);

    /* HW_MIDI_OUT_SIZE = 80 bytes = 20 packets of 4 bytes. Real events
     * have a non-zero header (cable+CIN); slot 0x00 marks end-of-events. */
    for (int i = 0; i < HW_MIDI_OUT_SIZE; i += 4) {
        if (hw_midi_out[i] == 0) break;
        test_stream_event_t *ev = &shm->buffer[seq % TEST_STREAM_CAPACITY];
        ev->frame  = frame;
        ev->pkt[0] = hw_midi_out[i];
        ev->pkt[1] = hw_midi_out[i + 1];
        ev->pkt[2] = hw_midi_out[i + 2];
        ev->pkt[3] = hw_midi_out[i + 3];
        seq++;
    }

    /* Single RELEASE store publishes all events written this frame.
     * Daemon's ACQUIRE load sees them as visible together. */
    __atomic_store_n(&shm->write_seq, seq, __ATOMIC_RELEASE);
}
