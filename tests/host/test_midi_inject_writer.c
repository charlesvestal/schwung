/*
 * Host-side unit tests for shadow_midi_inject_writer.h.
 *
 * Runs on the dev machine (Mac/Linux), not on Move. Validates the MPSC
 * helper standalone — no SHM, no shim, no QuickJS — by stack-allocating
 * a shadow_midi_inject_t and pounding it with single-threaded and
 * multi-threaded producers.
 *
 * Build + run:
 *   cc tests/host/test_midi_inject_writer.c -Isrc/host -lpthread \
 *      -O2 -Wall -Wextra -Wno-unused-parameter -o /tmp/test_inject \
 *   && /tmp/test_inject
 *
 * Or via the Makefile in this directory: `make -C tests/host test`.
 */

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shadow_midi_inject_writer.h"

/* ------------------------------------------------------------------ */

static void test_single_push(void) {
    shadow_midi_inject_t shm = {0};
    uint8_t pkt[4] = {0x09, 0x90, 60, 100};

    int rc = shadow_midi_inject_push(&shm, pkt);
    assert(rc == 0);

    assert(shm.alloc_idx == 4);
    assert(shm.commit_idx == 4);
    assert(shm.ready == 1);
    assert(memcmp(shm.buffer, pkt, 4) == 0);

    printf("  test_single_push                     PASS\n");
}

static void test_sequential_pushes_advance_cursors(void) {
    shadow_midi_inject_t shm = {0};

    for (int i = 0; i < 10; i++) {
        uint8_t pkt[4] = {0x09, 0x90, (uint8_t)(60 + i), 100};
        assert(shadow_midi_inject_push(&shm, pkt) == 0);
        assert(shm.alloc_idx == (i + 1) * 4);
        assert(shm.commit_idx == (i + 1) * 4);
        assert(shm.ready == (uint8_t)(i + 1));
        /* Note byte == 60+i is at offset (i*4 + 2) */
        assert(shm.buffer[i * 4 + 2] == (uint8_t)(60 + i));
    }

    printf("  test_sequential_pushes_advance       PASS\n");
}

static void test_buffer_full_rejection(void) {
    shadow_midi_inject_t shm = {0};
    uint8_t pkt[4] = {0x09, 0x90, 60, 100};

    /* Buffer holds 256 bytes / 4 per packet = 64 slots, but the bounds
     * check uses `>= 256` so the last legal slot start is 252. That
     * gives 63 successful pushes, then -1. */
    int n_ok = 0;
    int rc;
    while ((rc = shadow_midi_inject_push(&shm, pkt)) == 0) n_ok++;
    assert(rc == -1);
    assert(n_ok == 63);
    assert(shm.alloc_idx == 252);
    assert(shm.commit_idx == 252);

    printf("  test_buffer_full_rejection           PASS\n");
}

/* ------------------------------------------------------------------ */
/* Concurrent producers                                                */
/* ------------------------------------------------------------------ */

#define N_THREADS 8
#define WRITES_PER_THREAD 7  /* 8 * 7 = 56 packets, fits in 63-slot buffer */

typedef struct {
    shadow_midi_inject_t *shm;
    uint8_t marker;       /* unique per thread, written into pkt[2] */
    int stranded_count;   /* how many pushes returned -2 */
} writer_arg_t;

static void *writer_thread(void *arg_) {
    writer_arg_t *arg = arg_;
    uint8_t pkt[4] = {0x09, 0x90, arg->marker, 100};
    for (int i = 0; i < WRITES_PER_THREAD; i++) {
        int rc = shadow_midi_inject_push(arg->shm, pkt);
        if (rc == 0) continue;
        if (rc == -2) {
            arg->stranded_count++;
            continue;
        }
        fprintf(stderr, "writer %u: unexpected rc=%d on iter %d\n",
                arg->marker, rc, i);
        exit(1);
    }
    return NULL;
}

static void test_concurrent_no_collision(void) {
    shadow_midi_inject_t shm = {0};
    pthread_t threads[N_THREADS];
    writer_arg_t args[N_THREADS];

    for (int i = 0; i < N_THREADS; i++) {
        args[i] = (writer_arg_t){
            .shm = &shm,
            .marker = (uint8_t)(0x10 + i),
            .stranded_count = 0,
        };
        int err = pthread_create(&threads[i], NULL, writer_thread, &args[i]);
        assert(err == 0);
    }
    for (int i = 0; i < N_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Total successful pushes = N_THREADS * WRITES_PER_THREAD - stranded */
    int total_stranded = 0;
    for (int i = 0; i < N_THREADS; i++) total_stranded += args[i].stranded_count;
    int total_committed = N_THREADS * WRITES_PER_THREAD - total_stranded;

    /* Cursors must equal 4 * total_committed (each push advances by 4
     * on success) plus 4 * total_stranded on alloc_idx (reservations
     * succeeded but never committed). */
    int total_reserved = N_THREADS * WRITES_PER_THREAD;
    assert(shm.alloc_idx == total_reserved * 4);
    /* commit_idx may stop short if any thread stranded — but only if
     * the strand happened MID-sequence. With our spin limit and the
     * realistic test load, stranding should be near-zero. */
    if (total_stranded == 0) {
        assert(shm.commit_idx == total_committed * 4);
        assert(shm.ready == (uint8_t)total_committed);
    } else {
        printf("    (note: %d packets stranded — unusual under test load)\n",
               total_stranded);
    }

    /* Every committed slot must contain a real marker byte (0x10..0x17),
     * never zero. This is the smoking-gun assertion for the bug we're
     * fixing: with the old racy writer, two threads could clobber the
     * same slot and one packet would be silently lost — observable here
     * as a slot containing 0x00 in pkt[2] or pkt[0]. */
    for (int i = 0; i < total_committed; i++) {
        uint8_t cin    = shm.buffer[i * 4 + 0];
        uint8_t status = shm.buffer[i * 4 + 1];
        uint8_t note   = shm.buffer[i * 4 + 2];
        uint8_t vel    = shm.buffer[i * 4 + 3];
        assert(cin == 0x09);
        assert(status == 0x90);
        assert(note >= 0x10 && note <= (0x10 + N_THREADS - 1));
        assert(vel == 100);
    }

    /* And every thread's writes are accounted for in some order. Count
     * how many slots carry each marker; should equal WRITES_PER_THREAD
     * minus that thread's stranded_count. */
    int per_thread_seen[N_THREADS] = {0};
    for (int i = 0; i < total_committed; i++) {
        uint8_t marker = shm.buffer[i * 4 + 2];
        per_thread_seen[marker - 0x10]++;
    }
    for (int t = 0; t < N_THREADS; t++) {
        int expected = WRITES_PER_THREAD - args[t].stranded_count;
        if (per_thread_seen[t] != expected) {
            fprintf(stderr,
                    "thread %d: expected %d packets, saw %d\n",
                    t, expected, per_thread_seen[t]);
            assert(0);
        }
    }

    printf("  test_concurrent_no_collision         PASS\n");
}

/* ------------------------------------------------------------------ */
/* Stress: repeated runs to catch low-probability races                */
/* ------------------------------------------------------------------ */

static void test_stress_concurrent(void) {
    /* Re-run the concurrent case 50× to flush out timing-sensitive bugs
     * that only fire on rare interleavings. Total ~50 * 56 = 2800
     * packets, all should commit cleanly with no clobbering. */
    for (int run = 0; run < 50; run++) {
        shadow_midi_inject_t shm = {0};
        pthread_t threads[N_THREADS];
        writer_arg_t args[N_THREADS];
        for (int i = 0; i < N_THREADS; i++) {
            args[i] = (writer_arg_t){.shm = &shm,
                                     .marker = (uint8_t)(0x10 + i)};
            pthread_create(&threads[i], NULL, writer_thread, &args[i]);
        }
        for (int i = 0; i < N_THREADS; i++) pthread_join(threads[i], NULL);

        int total = 0;
        for (int t = 0; t < N_THREADS; t++)
            total += WRITES_PER_THREAD - args[t].stranded_count;

        for (int i = 0; i < total; i++) {
            assert(shm.buffer[i * 4 + 0] == 0x09);
            uint8_t marker = shm.buffer[i * 4 + 2];
            assert(marker >= 0x10 && marker < 0x10 + N_THREADS);
        }
    }
    printf("  test_stress_concurrent (x50)         PASS\n");
}

/* ------------------------------------------------------------------ */

int main(void) {
    printf("shadow_midi_inject_writer tests:\n");
    test_single_push();
    test_sequential_pushes_advance_cursors();
    test_buffer_full_rejection();
    test_concurrent_no_collision();
    test_stress_concurrent();
    printf("ALL PASS\n");
    return 0;
}
