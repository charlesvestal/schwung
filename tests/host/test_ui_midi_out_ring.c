/*
 * SPSC discipline for /schwung-midi-out.
 *
 * The bug: shadow_inject_ui_midi_out() (the shim, SPI callback) snapshotted
 * write_idx, memcpy'd, then set `write_idx = 0` AND memset the whole buffer —
 * while js_shadow_midi_send() (shadow_ui, a SEPARATE PROCESS) was appending at
 * write_idx and returning JS_TRUE. A packet written in that window is erased
 * and its index discarded, after the sender has been told it was sent.
 *
 * Invisible to every counter on either side. One lost packet inside a SysEx
 * run corrupts the whole message, which the E16 renders as a garbled screen.
 *
 * These tests drive the real header, so they fail if the consumer is ever
 * given back the right to write the producer's region — the property, not the
 * spelling of the fix.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ui_midi_out_ring.h"

static int failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

/* A packet whose four bytes encode `n`, so a reordering or a partial copy is
 * detectable rather than merely "different". */
static void fill_packet(uint8_t p[4], int n) {
    p[0] = (uint8_t)(n & 0xFF);
    p[1] = (uint8_t)((n >> 8) & 0xFF);
    p[2] = (uint8_t)(n ^ 0x5A);
    p[3] = (uint8_t)(~n & 0xFF);
}

static int packet_is(const uint8_t *p, int n) {
    uint8_t want[4];
    fill_packet(want, n);
    return memcmp(p, want, 4) == 0;
}

static shadow_midi_out_t ring;

static void reset(void) {
    memset(&ring, 0, sizeof(ring));
}

/* Push one packet numbered `n`. */
static int push_one(int n) {
    uint8_t p[4];
    fill_packet(p, n);
    return ui_midi_out_push(&ring, p, 4);
}

int main(void) {
    printf("test_ui_midi_out_ring\n");

    /* ---- empty vs full are distinguishable ------------------------------ */
    reset();
    check(ui_midi_out_used(&ring) == 0, "a zeroed ring is empty");
    check(ui_midi_out_free(&ring) == UI_MIDI_OUT_CAPACITY,
          "a zeroed ring is entirely free");

    int pushed = 0;
    while (push_one(pushed)) pushed++;
    check(pushed == UI_MIDI_OUT_CAPACITY / 4,
          "the ring accepts exactly capacity/4 packets");
    check(ui_midi_out_used(&ring) == UI_MIDI_OUT_CAPACITY,
          "a full ring reports capacity used");
    check(ui_midi_out_used(&ring) != 0,
          "FULL IS NOT EMPTY -- the reserved packet is what tells them apart");
    check(ui_midi_out_free(&ring) == 0, "a full ring has no room");

    /* ---- order and content survive a drain ------------------------------ */
    {
        uint16_t len = ui_midi_out_used(&ring);
        static uint8_t out[SHADOW_MIDI_OUT_BUFFER_SIZE];
        ui_midi_out_copy(&ring, out, len);
        int ok = 1;
        for (int i = 0; i < len / 4; i++)
            if (!packet_is(&out[i * 4], i)) { ok = 0; break; }
        check(ok, "every packet comes back in the order it was pushed");
        ui_midi_out_commit(&ring, len);
        check(ui_midi_out_used(&ring) == 0, "committing the whole run empties it");
    }

    /* ---- the wrap is exact ---------------------------------------------- */
    /* Drive the indices right up to the uint16 wrap and across it. 65536 is a
     * whole multiple of the buffer, so the distance arithmetic must stay exact
     * there; if it were not, the ring would lose a message per wrap and look
     * fine the rest of the time. */
    {
        reset();
        int n = 0;
        int drained = 0;
        int order_ok = 1;
        /* Push and drain a packet at a time for well past 65536 bytes, so
         * every index wraps and the buffer wraps sixteen times over. */
        for (int step = 0; step < 40000; step++) {
            if (!push_one(n)) { check(0, "steady-state push refused"); break; }
            n++;
            uint16_t len = ui_midi_out_used(&ring);
            if (len != 4) { check(0, "used() is wrong at the wrap"); break; }
            uint8_t out[4];
            ui_midi_out_copy(&ring, out, 4);
            if (!packet_is(out, drained)) { order_ok = 0; break; }
            ui_midi_out_commit(&ring, 4);
            drained++;
        }
        check(order_ok && drained == 40000,
              "40000 packets cross the uint16 wrap with content intact");
    }

    /* ---- a straddling run is copied whole ------------------------------- */
    /* Position the write cursor near the end of the buffer, then push a run
     * long enough to wrap. A copy that forgot the second memcpy would return
     * the right LENGTH with the tail full of stale bytes -- which is exactly
     * the shape of a garbled screen, so the test checks content, not size. */
    {
        reset();
        /* Fill, then drain, to leave both indices near the buffer's end. */
        int near_end = (SHADOW_MIDI_OUT_BUFFER_SIZE - 12) / 4;
        for (int i = 0; i < near_end; i++) push_one(i);
        ui_midi_out_commit(&ring, ui_midi_out_used(&ring));
        check(((uint16_t)ring.write_idx & UI_MIDI_OUT_MASK) ==
                  SHADOW_MIDI_OUT_BUFFER_SIZE - 12,
              "the cursor is parked 3 packets from the end");

        uint8_t run[40];
        for (int i = 0; i < 10; i++) fill_packet(&run[i * 4], 1000 + i);
        check(ui_midi_out_push(&ring, run, sizeof(run)) == 1,
              "a 10-packet run straddling the wrap is accepted");

        uint8_t out[40];
        check(ui_midi_out_used(&ring) == sizeof(run),
              "a straddling run reports its whole length");
        ui_midi_out_copy(&ring, out, sizeof(out));
        check(memcmp(run, out, sizeof(run)) == 0,
              "a straddling run comes back byte-for-byte -- both halves");
    }

    /* ---- all or nothing -------------------------------------------------- */
    {
        reset();
        /* Leave room for exactly two packets. */
        int fill = UI_MIDI_OUT_CAPACITY / 4 - 2;
        for (int i = 0; i < fill; i++) push_one(i);
        check(ui_midi_out_free(&ring) == 8, "room for two packets");

        uint8_t run[12];
        for (int i = 0; i < 3; i++) fill_packet(&run[i * 4], 900 + i);
        uint16_t before = ui_midi_out_used(&ring);
        check(ui_midi_out_push(&ring, run, sizeof(run)) == 0,
              "a 3-packet run is refused when only 2 fit");
        check(ui_midi_out_used(&ring) == before,
              "A REFUSED RUN LEAVES NOTHING BEHIND -- a partial write would be "
              "a truncated SysEx on the wire");
    }

    /* ---- deferring costs nothing ---------------------------------------- */
    /* The consumer copies without committing when the carry cannot take the
     * whole run. The bytes must still be there on the next attempt: that is
     * what makes a deferral a deferral rather than a drop. */
    {
        reset();
        for (int i = 0; i < 5; i++) push_one(i);
        uint16_t len = ui_midi_out_used(&ring);
        uint8_t first[20], second[20];
        ui_midi_out_copy(&ring, first, len);
        /* ...decline to commit, as the capacity check does... */
        check(ui_midi_out_used(&ring) == len,
              "a copy without a commit consumes nothing");
        ui_midi_out_copy(&ring, second, len);
        check(memcmp(first, second, len) == 0,
              "the deferred snapshot is the same bytes on the retry");
    }

    /* ---- the consumer's only write is read_idx --------------------------- */
    /* The property, stated as a memory fact rather than as a code shape: run a
     * consume against a ring whose buffer and write_idx are known, and assert
     * that neither moved. A memset or a `write_idx = 0` creeping back in fails
     * here whatever it is spelled. */
    {
        reset();
        for (int i = 0; i < 7; i++) push_one(i);
        shadow_midi_out_t before;
        memcpy(&before, &ring, sizeof(before));

        uint16_t len = ui_midi_out_used(&ring);
        uint8_t out[28];
        ui_midi_out_copy(&ring, out, len);
        ui_midi_out_commit(&ring, len);

        check(memcmp(before.buffer, ring.buffer, sizeof(ring.buffer)) == 0,
              "THE CONSUMER DOES NOT TOUCH THE BUFFER -- the memset that erased "
              "packets the producer had just written is gone");
        check((uint16_t)before.write_idx == (uint16_t)ring.write_idx,
              "THE CONSUMER DOES NOT TOUCH write_idx -- the `write_idx = 0` that "
              "discarded a concurrent append's index is gone");
        check((uint16_t)ring.read_idx == (uint16_t)before.read_idx + len,
              "read_idx advanced by exactly what was taken");
    }

    /* ---- a stale index from either side is safe -------------------------- */
    /* Each process reads the other's index without a lock. A stale read is
     * always the OLDER value, so the producer sees less free space and the
     * consumer sees fewer bytes -- conservative in both directions, never
     * over-optimistic. Model it: consume against a snapshot of write_idx taken
     * before a concurrent push, and the pushed packet must survive. */
    {
        reset();
        for (int i = 0; i < 3; i++) push_one(i);
        uint16_t stale_len = ui_midi_out_used(&ring);   /* consumer's snapshot */

        push_one(99);                                    /* producer, concurrently */

        uint8_t out[12];
        ui_midi_out_copy(&ring, out, stale_len);
        ui_midi_out_commit(&ring, stale_len);

        check(ui_midi_out_used(&ring) == 4,
              "a packet appended after the consumer's snapshot SURVIVES -- this "
              "is the packet the old memset erased while reporting success");
        uint8_t late[4];
        ui_midi_out_copy(&ring, late, 4);
        check(packet_is(late, 99), "and it is the right packet, uncorrupted");
    }

    if (failures) {
        printf("FAILED: %d check(s)\n", failures);
        return 1;
    }
    printf("PASS\n");
    return 0;
}
