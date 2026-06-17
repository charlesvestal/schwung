/*
 * Host-side unit tests for shadow_midi_inject_writer.h (bounded MPSC ring).
 *
 * Runs on the dev machine (Mac/Linux), not on Move. Validates the ring
 * standalone — no SHM, no shim, no QuickJS — by stack-allocating a
 * shadow_midi_inject_t and exercising producers AND a concurrent consumer.
 *
 * The concurrent-consumer tests are the point: the bug class this ring
 * fixes (Move SIGABRT from a torn / cable=0/CIN=0 packet) only manifests
 * when producers push WHILE the consumer drains and frees slots. A
 * producer-vs-producer-only test cannot reproduce it.
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
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shadow_midi_inject_writer.h"

/* ------------------------------------------------------------------ *
 * Packet encoding: byte 0 is a fixed non-zero CIN (0x09 = note-on).
 * The remaining 3 bytes carry a 24-bit unique id so the consumer can
 * verify exactly-once delivery. byte 0 == 0 is the SIGABRT smoking gun
 * (cable=0/CIN=0 → Move reads "misc function" → abort), so every
 * consumed packet MUST have byte 0 == 0x09.
 * ------------------------------------------------------------------ */
static inline void encode_pkt(uint8_t pkt[4], uint32_t id) {
    pkt[0] = 0x09;
    pkt[1] = (uint8_t)(id >> 16);
    pkt[2] = (uint8_t)(id >> 8);
    pkt[3] = (uint8_t)(id);
}
static inline uint32_t decode_id(const uint8_t pkt[4]) {
    return ((uint32_t)pkt[1] << 16) | ((uint32_t)pkt[2] << 8) | pkt[3];
}

/* ------------------------------------------------------------------ */
/* Single-threaded correctness                                         */
/* ------------------------------------------------------------------ */

static void test_single_push_peek_pop(void) {
    shadow_midi_inject_t shm;
    shadow_midi_inject_init(&shm);

    uint8_t pkt[4], out[4];
    encode_pkt(pkt, 0xABCDE);

    assert(shadow_midi_inject_peek(&shm, out) == 0);  /* empty */
    assert(shadow_midi_inject_push(&shm, pkt) == 0);
    assert(shadow_midi_inject_peek(&shm, out) == 1);
    assert(memcmp(out, pkt, 4) == 0);
    /* peek does not consume — peeking again yields the same packet */
    assert(shadow_midi_inject_peek(&shm, out) == 1);
    assert(memcmp(out, pkt, 4) == 0);
    shadow_midi_inject_pop(&shm);
    assert(shadow_midi_inject_peek(&shm, out) == 0);  /* empty again */

    printf("  test_single_push_peek_pop            PASS\n");
}

static void test_sequential_fifo(void) {
    shadow_midi_inject_t shm;
    shadow_midi_inject_init(&shm);

    for (uint32_t i = 0; i < 10; i++) {
        uint8_t pkt[4]; encode_pkt(pkt, 1000 + i);
        assert(shadow_midi_inject_push(&shm, pkt) == 0);
    }
    for (uint32_t i = 0; i < 10; i++) {
        uint8_t out[4];
        assert(shadow_midi_inject_peek(&shm, out) == 1);
        assert(out[0] == 0x09);
        assert(decode_id(out) == 1000 + i);  /* FIFO order */
        shadow_midi_inject_pop(&shm);
    }
    assert(shadow_midi_inject_peek(&shm, (uint8_t[4]){0}) == 0);

    printf("  test_sequential_fifo                 PASS\n");
}

static void test_full_capacity_and_recovery(void) {
    shadow_midi_inject_t shm;
    shadow_midi_inject_init(&shm);

    /* A bounded MPSC ring holds exactly SLOTS items when empty — not
     * SLOTS-1. (The prior byte-ring's `>= SIZE` bound lost one slot.) */
    uint8_t pkt[4]; encode_pkt(pkt, 1);
    int n_ok = 0, rc;
    while ((rc = shadow_midi_inject_push(&shm, pkt)) == 0) n_ok++;
    assert(rc == -1);
    assert(n_ok == SHADOW_MIDI_INJECT_SLOTS);  /* full capacity, no off-by-one */

    /* Drain a few, then pushes succeed again (slots recycle). */
    uint8_t out[4];
    for (int i = 0; i < 3; i++) { assert(shadow_midi_inject_peek(&shm, out) == 1); shadow_midi_inject_pop(&shm); }
    for (int i = 0; i < 3; i++) assert(shadow_midi_inject_push(&shm, pkt) == 0);
    assert(shadow_midi_inject_push(&shm, pkt) == -1);  /* full again */

    printf("  test_full_capacity_and_recovery      PASS\n");
}

/* ------------------------------------------------------------------ */
/* Concurrent producers + concurrent consumer (the real hazard)        */
/* ------------------------------------------------------------------ */

#define N_THREADS         8
#define WRITES_PER_THREAD 4000           /* 8 * 4000 = 32000 packets total */
#define TOTAL_PACKETS     (N_THREADS * WRITES_PER_THREAD)
#define PUSH_RETRY_CAP    200000000      /* sanity bound; never hit if healthy */

typedef struct {
    shadow_midi_inject_t *shm;
    uint32_t base_id;    /* this thread owns ids [base_id, base_id + WRITES_PER_THREAD) */
    long     retries;    /* how many -1 (full) we spun through */
} writer_arg_t;

static void *writer_thread(void *arg_) {
    writer_arg_t *arg = arg_;
    for (uint32_t i = 0; i < WRITES_PER_THREAD; i++) {
        uint8_t pkt[4]; encode_pkt(pkt, arg->base_id + i);
        long attempts = 0;
        while (shadow_midi_inject_push(arg->shm, pkt) != 0) {
            /* Ring momentarily full — the consumer will free a slot. Retry.
             * -1 is the ONLY failure code; it must clear, never wedge. */
            if (++attempts > PUSH_RETRY_CAP) {
                fprintf(stderr, "writer base=%u stuck at i=%u (ring never drained)\n",
                        arg->base_id, i);
                exit(1);
            }
            sched_yield();
        }
        arg->retries += attempts;
    }
    return NULL;
}

typedef struct {
    shadow_midi_inject_t *shm;
    uint8_t *seen;        /* seen[id]++ — consumer-only, so plain array is fine */
    long     consumed;
    int      done;        /* set by main once producers have joined */
    int      torn;        /* count of torn/garbage packets (must stay 0) */
} reader_arg_t;

static void *reader_thread(void *arg_) {
    reader_arg_t *arg = arg_;
    for (;;) {
        uint8_t out[4];
        if (shadow_midi_inject_peek(arg->shm, out)) {
            /* Smoking gun: a torn / partially-written packet would show
             * byte 0 == 0 (the cable=0/CIN=0 that aborts Move firmware),
             * or an id outside the produced range. */
            if (out[0] != 0x09) { arg->torn++; }
            uint32_t id = decode_id(out);
            if (id >= TOTAL_PACKETS) { arg->torn++; }
            else { arg->seen[id]++; }
            shadow_midi_inject_pop(arg->shm);
            arg->consumed++;
        } else if (__atomic_load_n(&arg->done, __ATOMIC_ACQUIRE) &&
                   !shadow_midi_inject_peek(arg->shm, out)) {
            /* Producers finished and the ring is drained → we're done. */
            break;
        } else {
            sched_yield();
        }
    }
    return NULL;
}

static void run_concurrent_case(int print) {
    shadow_midi_inject_t shm;
    shadow_midi_inject_init(&shm);

    uint8_t *seen = calloc(TOTAL_PACKETS, 1);
    assert(seen);

    pthread_t producers[N_THREADS], consumer;
    writer_arg_t wargs[N_THREADS];
    reader_arg_t rarg = { .shm = &shm, .seen = seen, .consumed = 0, .done = 0, .torn = 0 };

    /* Start the consumer first so it's already draining as producers ramp. */
    assert(pthread_create(&consumer, NULL, reader_thread, &rarg) == 0);
    for (int i = 0; i < N_THREADS; i++) {
        wargs[i] = (writer_arg_t){ .shm = &shm,
                                   .base_id = (uint32_t)i * WRITES_PER_THREAD,
                                   .retries = 0 };
        assert(pthread_create(&producers[i], NULL, writer_thread, &wargs[i]) == 0);
    }
    for (int i = 0; i < N_THREADS; i++) pthread_join(producers[i], NULL);
    __atomic_store_n(&rarg.done, 1, __ATOMIC_RELEASE);
    pthread_join(consumer, NULL);

    /* No torn / garbage packets ever reached the consumer. */
    assert(rarg.torn == 0);
    /* Exactly-once delivery: every produced id consumed exactly once. */
    assert(rarg.consumed == TOTAL_PACKETS);
    long missing = 0, dup = 0;
    for (long id = 0; id < TOTAL_PACKETS; id++) {
        if (seen[id] == 0) missing++;
        else if (seen[id] > 1) dup++;
    }
    if (missing || dup) {
        fprintf(stderr, "delivery error: missing=%ld duplicated=%ld\n", missing, dup);
        assert(0);
    }

    long total_retries = 0;
    for (int i = 0; i < N_THREADS; i++) total_retries += wargs[i].retries;
    if (print) {
        printf("  test_concurrent_producers_consumer   PASS  (%d pkts, %ld full-retries)\n",
               TOTAL_PACKETS, total_retries);
    }
    free(seen);
}

static void test_concurrent_producers_consumer(void) {
    run_concurrent_case(1);
}

/* Repeat the concurrent case to shake out rare interleavings. */
static void test_stress_concurrent(void) {
    for (int run = 0; run < 20; run++) {
        run_concurrent_case(0);
    }
    printf("  test_stress_concurrent (x20)         PASS\n");
}

/* ------------------------------------------------------------------ */

int main(void) {
    printf("shadow_midi_inject_writer tests (Vyukov MPSC):\n");
    test_single_push_peek_pop();
    test_sequential_fifo();
    test_full_capacity_and_recovery();
    test_concurrent_producers_consumer();
    test_stress_concurrent();
    printf("ALL PASS\n");
    return 0;
}
