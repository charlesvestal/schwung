#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shadow_midi_filter.h"   /* SHADOW_MIDI_IN_BYTES */
#include "shadow_midi_inject_writer.h"
#include "shadow_overtake_midi.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

static void test_active_overtake_uses_only_dedicated_queue(void)
{
    shadow_midi_inject_t shared;
    uint8_t midi_in[31 * 8];
    uint8_t shared_pkt[4] = { 0x29, 0x90, 72, 100 };
    uint8_t dsp_on[4] = { 0x29, 0x90, 60, 100 };
    uint8_t dsp_off[4] = { 0x28, 0x80, 60, 0 };
    uint8_t peek[4];

    shadow_midi_inject_init(&shared);
    shadow_overtake_midi_init();
    memset(midi_in, 0, sizeof(midi_in));

    CHECK(shadow_midi_inject_push(&shared, shared_pkt) == 0,
          "shared test-bus packet queued");
    CHECK(shadow_overtake_midi_send(dsp_on, 4) == 4,
          "overtake note-on queued");
    CHECK(shadow_overtake_midi_send(dsp_off, 4) == 4,
          "overtake note-off queued");

    CHECK(shadow_overtake_midi_drain(&shared, 1, midi_in, 31, sizeof(midi_in)) == 2,
          "active overtake drains both dedicated packets");
    CHECK(memcmp(&midi_in[0], dsp_on, 4) == 0,
          "dedicated note-on is first");
    CHECK(memcmp(&midi_in[8], dsp_off, 4) == 0,
          "dedicated note-off is second");
    CHECK(midi_in[4] == 0 && midi_in[12] == 0,
          "injected timestamps are zero");
    CHECK(shadow_midi_inject_peek(&shared, peek) == 1
              && memcmp(peek, shared_pkt, 4) == 0,
          "active overtake leaves shared queue for its publisher");

    memset(midi_in, 0, sizeof(midi_in));
    CHECK(shadow_overtake_midi_drain(&shared, 0, midi_in, 31, sizeof(midi_in)) == 1,
          "inactive mode resumes the shared queue");
    CHECK(memcmp(&midi_in[0], shared_pkt, 4) == 0,
          "shared packet reaches Move after overtake exits");
    CHECK(shadow_midi_inject_peek(&shared, peek) == 0,
          "shared packet is consumed only after it is copied");
}

static void test_full_queue_preserves_fifo(void)
{
    uint8_t midi_in[8];
    uint8_t packet[4] = { 0x29, 0x90, 0, 100 };

    shadow_overtake_midi_init();
    for (int i = 0; i < SHADOW_MIDI_INJECT_SLOTS; i++) {
        /* Note 1 upward, never 0. The oldest packet is what this asserts on,
         * and 0 is also what memset leaves in the buffer -- an assertion whose
         * expected value equals the uninitialised value cannot fail. */
        packet[2] = (uint8_t)(i + 1);
        CHECK(shadow_overtake_midi_send(packet, 4) == 4,
              "dedicated ring accepts every available slot");
    }
    packet[2] = 127;
    CHECK(shadow_overtake_midi_send(packet, 4) == 0,
          "dedicated ring rejects a packet when full");

    memset(midi_in, 0, sizeof(midi_in));
    CHECK(shadow_overtake_midi_drain(NULL, 1, midi_in, 1, sizeof(midi_in)) == 1,
          "bounded drain copies only the requested packet count");
    CHECK(midi_in[2] == 1,
          "ring overflow does not disturb the oldest packet");
}

/* An unloaded module's leftovers must not be replayed into Move by whatever
 * loads next. The ring is a shim-lifetime static, so "the instance went away"
 * does not empty it. */
static void test_unload_discards_the_departed_modules_packets(void)
{
    uint8_t midi_in[SHADOW_MIDI_IN_BYTES];
    uint8_t note[4] = { 0x29, 0x90, 64, 100 };

    shadow_overtake_midi_init();
    CHECK(shadow_overtake_midi_send(note, 4) == 4, "a module queues a note");

    shadow_overtake_midi_discard();

    memset(midi_in, 0, sizeof(midi_in));
    CHECK(shadow_overtake_midi_drain(NULL, 1, midi_in, 31, sizeof(midi_in)) == 0,
          "after unload the ring has nothing to replay");
    CHECK(midi_in[0] == 0,
          "and the next module's first frame is clean");
}

/* The shim's overtake-exit releases (shift/vol/back/jog off) go into the
 * SHARED ring at the overtake->0 edge. A dropped release leaves Move believing
 * a control is still held, so they must not queue behind a departing DSP's
 * leftovers. */
static void test_shared_ring_outranks_the_dedicated_one_on_exit(void)
{
    shadow_midi_inject_t shared;
    uint8_t midi_in[SHADOW_MIDI_IN_BYTES];
    uint8_t release[4] = { 0x0B, 0xB0, 49, 0 };    /* shift off */
    uint8_t leftover[4] = { 0x29, 0x90, 60, 100 };

    shadow_midi_inject_init(&shared);
    shadow_overtake_midi_init();
    memset(midi_in, 0, sizeof(midi_in));

    /* Dedicated queued FIRST, so passing this cannot be an accident of order. */
    CHECK(shadow_overtake_midi_send(leftover, 4) == 4, "DSP leftover queued");
    CHECK(shadow_midi_inject_push(&shared, release) == 0, "release queued");

    CHECK(shadow_overtake_midi_drain(&shared, 0, midi_in, 31, sizeof(midi_in)) == 2,
          "both drain once overtake is over");
    CHECK(memcmp(&midi_in[0], release, 4) == 0,
          "the RELEASE goes out first, ahead of the departed DSP's note");
}

/* CLAUDE.md: bound every 8-stride walk with SHADOW_MIDI_IN_BYTES. The RX
 * display-status word sits at +248, so a caller passing a generous event count
 * against a short buffer must not be believed. */
static void test_drain_is_bounded_by_bytes_not_just_events(void)
{
    uint8_t guarded[SHADOW_MIDI_IN_STRIDE * 3 + 8];
    uint8_t packet[4] = { 0x29, 0x90, 70, 100 };
    const int usable = SHADOW_MIDI_IN_STRIDE * 3;

    shadow_overtake_midi_init();
    for (int i = 0; i < 10; i++) {
        packet[2] = (uint8_t)(70 + i);
        shadow_overtake_midi_send(packet, 4);
    }

    memset(guarded, 0, sizeof(guarded));
    /* 31 events claimed, 3 slots of room. The byte bound is the only thing
     * standing between this and the word past the end. */
    int n = shadow_overtake_midi_drain(NULL, 1, guarded, 31, usable);
    CHECK(n == 3, "the byte bound wins over the event count");

    int past_end_clean = 1;
    for (int i = usable; i < (int)sizeof(guarded); i++)
        if (guarded[i] != 0) past_end_clean = 0;
    CHECK(past_end_clean, "nothing was written past the buffer");
}

int main(void)
{
    test_active_overtake_uses_only_dedicated_queue();
    test_full_queue_preserves_fifo();
    test_unload_discards_the_departed_modules_packets();
    test_shared_ring_outranks_the_dedicated_one_on_exit();
    test_drain_is_bounded_by_bytes_not_just_events();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS: overtake DSP MIDI reaches Move without consuming the shared test bus\n");
    return 0;
}
