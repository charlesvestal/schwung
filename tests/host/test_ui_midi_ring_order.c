/*
 * The shim -> shadow_ui MIDI channel delivers IN ARRIVAL ORDER.
 *
 * Measured on hardware 2026-09-24: the E16 ACKed 38 of 38 OLED CLEARs on the
 * wire and not one reached JS. The producer took the LOWEST free slot and the
 * consumer drained in INDEX order, so a burst that straddled a drain was read
 * shuffled -- fatal to SysEx. This drives exactly that interleaving: the
 * consumer drains part of the ring, the producer writes more, and every packet
 * must still come out in the order it went in. It also covers the resync after
 * a consumer restart (cursors in two processes) and a full ring (drop, never
 * overwrite an unread slot).
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "ui_midi_ring.h"

#define BYTES 64            /* 16 slots: small, so wrap happens often */
static uint8_t ring[BYTES];
static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL %s\n", m); fails++; } else printf("ok   %s\n", m); } while (0)

/* Consume up to `max` packets; append their d1 (the sequence number). */
static int drain(int *rd, int max, uint8_t *out, int *n) {
    int got = 0;
    while (got < max) {
        int i = ui_midi_ring_next(ring, BYTES, rd);
        if (i < 0) break;
        ui_midi_ring_advance(rd, BYTES);
        out[(*n)++] = ring[i + 2];
        __atomic_store_n(&ring[i], 0, __ATOMIC_RELEASE);
        got++;
    }
    return got;
}

int main(void) {
    int wr = 0, rd = 0, n = 0;
    uint8_t out[1024];
    uint8_t seq = 1;

    /* THE HARDWARE PATTERN: write a burst, drain part of it, write more (which
     * under lowest-free-slot landed in the slots just freed), drain the rest.
     * Repeated across many wraps. */
    for (int round = 0; round < 40; round++) {
        for (int k = 0; k < 7; k++) ui_midi_ring_put(ring, BYTES, &wr, 0x24, 0xF0, seq++, 0);
        drain(&rd, 3, out, &n);
        for (int k = 0; k < 5; k++) ui_midi_ring_put(ring, BYTES, &wr, 0x24, 0xF0, seq++, 0);
        drain(&rd, 100, out, &n);
    }
    int ordered = 1;
    for (int i = 1; i < n; i++) if ((uint8_t)(out[i - 1] + 1) != out[i]) { ordered = 0; break; }
    CHECK(n == 40 * 12, "every packet delivered across 40 interleaved rounds");
    CHECK(ordered, "...and every one in ARRIVAL order, across wraps");

    /* FULL: a producer a whole ring ahead drops, never overwrites unread. */
    memset(ring, 0, sizeof ring); wr = rd = 0; n = 0; seq = 1;
    int placed = 0;
    for (int k = 0; k < BYTES / 4 + 3; k++) placed += ui_midi_ring_put(ring, BYTES, &wr, 0x24, 0xF0, seq++, 0);
    CHECK(placed == BYTES / 4, "a full ring refuses rather than overwriting an unread slot");
    drain(&rd, 100, out, &n);
    CHECK(n == BYTES / 4 && out[0] == 1 && out[n - 1] == BYTES / 4, "...and the first ring's worth comes out intact, in order");

    /* RESYNC: the consumer restarts (cursor 0) while the producer kept its
     * cursor mid-ring. The next packets must still arrive, in order. */
    memset(ring, 0, sizeof ring); n = 0; seq = 1;
    wr = 9 * 4; rd = 0;
    for (int k = 0; k < 4; k++) ui_midi_ring_put(ring, BYTES, &wr, 0x24, 0xF0, seq++, 0);
    drain(&rd, 100, out, &n);
    CHECK(n == 4 && out[0] == 1 && out[3] == 4, "a restarted consumer resyncs to the producer run, in order");
    for (int k = 0; k < 3; k++) ui_midi_ring_put(ring, BYTES, &wr, 0x24, 0xF0, seq++, 0);
    drain(&rd, 100, out, &n);
    CHECK(n == 7 && out[4] == 5 && out[6] == 7, "...and stays in step afterwards");

    printf(fails ? "FAILED %d\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
