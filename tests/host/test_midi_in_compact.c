/* Regression test: a filtered MIDI_IN slot must not hide the events behind it.
 *
 * Move's firmware reads MIDI_IN until the first EMPTY slot (the Ableton SPI
 * convention — schwung_usb_midi_msg_is_empty: cable, CIN and all three payload
 * bytes zero).  The shim suppresses an event by zeroing its slot in place, so
 * a suppressed event is not a hole, it is a TERMINATOR.
 *
 * The bug that produced this test: with the shadow UI up, knob CCs (71-78) and
 * knob-touch notes (0-9) are filtered on every detent.  Spin a knob while pads
 * are held and a pad note-off landing in a later slot of the same frame never
 * reaches Move — Move keeps the pad lit and its own instrument sounding, and
 * because chain slots are fed from Move's MIDI_OUT echo, the slot synth never
 * sees the note-off either.  One drop, two stuck consumers, with the note-off
 * present and correctly ordered in the raw hardware mailbox the whole time.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shadow_midi_filter.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

/* Model Move's reader: walk from slot 0 and stop dead at the first empty
 * slot.  Returns how many events it got to see. */
static int move_sees(const uint8_t *midi_in)
{
    int n = 0;
    for (int j = 0; j < SHADOW_MIDI_IN_BYTES; j += SHADOW_MIDI_IN_STRIDE) {
        if (shadow_midi_in_slot_empty(&midi_in[j])) break;
        n++;
    }
    return n;
}

static void put(uint8_t *buf, int slot, uint8_t head, uint8_t status,
                uint8_t d1, uint8_t d2, uint8_t stamp)
{
    uint8_t *p = &buf[slot * SHADOW_MIDI_IN_STRIDE];
    p[0] = head; p[1] = status; p[2] = d1; p[3] = d2;
    p[4] = stamp; p[5] = 0; p[6] = 0; p[7] = 0;
}

static void clear_slot(uint8_t *buf, int slot)
{
    memset(&buf[slot * SHADOW_MIDI_IN_STRIDE], 0, 4);
}

/* THE BUG, and the fix. */
static void test_note_off_behind_a_filtered_knob(void)
{
    uint8_t buf[SHADOW_MIDI_IN_BYTES];
    memset(buf, 0, sizeof(buf));

    put(buf, 0, 0x0B, 0xB0, 71, 0x01, 0x11);   /* knob 1 detent  — filtered */
    put(buf, 1, 0x08, 0x80, 85, 0x40, 0x22);   /* pad 85 note-off — must survive */
    put(buf, 2, 0x0B, 0xB0, 72, 0x01, 0x33);   /* knob 2 detent  — filtered */
    put(buf, 3, 0x08, 0x80, 86, 0x40, 0x44);   /* pad 86 note-off — must survive */

    clear_slot(buf, 0);   /* what the shim's filter does */
    clear_slot(buf, 2);

    CHECK(move_sees(buf) == 0,
          "precondition: a zeroed slot 0 hides EVERY event behind it");

    CHECK(shadow_midi_in_compact(buf) == 2, "two events survive the filter");
    CHECK(move_sees(buf) == 2, "Move must see both surviving note-offs");

    CHECK(buf[0] == 0x08 && buf[2] == 85, "note-off 85 moved down to slot 0");
    CHECK(buf[4] == 0x22, "its timestamp travelled with it");
    CHECK(buf[8] == 0x08 && buf[10] == 86, "note-off 86 kept its order, slot 1");
    CHECK(buf[12] == 0x44, "and its timestamp too");
}

static void test_order_is_preserved_across_many_gaps(void)
{
    uint8_t buf[SHADOW_MIDI_IN_BYTES];
    memset(buf, 0, sizeof(buf));

    /* 10 events, every other one filtered out. */
    for (int i = 0; i < 10; i++)
        put(buf, i, 0x09, 0x90, (uint8_t)(60 + i), 100, (uint8_t)(i + 1));
    for (int i = 0; i < 10; i += 2)
        clear_slot(buf, i);

    CHECK(shadow_midi_in_compact(buf) == 5, "five survivors");
    CHECK(move_sees(buf) == 5, "Move sees all five");
    for (int i = 0; i < 5; i++) {
        const uint8_t *p = &buf[i * SHADOW_MIDI_IN_STRIDE];
        CHECK(p[2] == (uint8_t)(61 + 2 * i), "survivors kept their arrival order");
        CHECK(p[4] == (uint8_t)(2 + 2 * i), "each carried its own timestamp");
    }
}

/* A frame with nothing filtered must come out byte-identical — compaction runs
 * unconditionally on every SPI frame, so a no-op has to really be a no-op. */
static void test_dense_frame_is_untouched(void)
{
    uint8_t buf[SHADOW_MIDI_IN_BYTES], before[SHADOW_MIDI_IN_BYTES];
    memset(buf, 0, sizeof(buf));
    put(buf, 0, 0x09, 0x90, 68, 100, 0x01);
    put(buf, 1, 0x09, 0x90, 69, 100, 0x02);
    put(buf, 2, 0x08, 0x80, 68, 0x40, 0x03);
    memcpy(before, buf, sizeof(buf));

    CHECK(shadow_midi_in_compact(buf) == 3, "three events, no gaps");
    CHECK(memcmp(buf, before, sizeof(buf)) == 0,
          "a gapless frame must come out byte-identical");
}

static void test_empty_and_full_frames(void)
{
    uint8_t buf[SHADOW_MIDI_IN_BYTES];

    memset(buf, 0, sizeof(buf));
    CHECK(shadow_midi_in_compact(buf) == 0, "an empty frame compacts to zero events");
    CHECK(move_sees(buf) == 0, "and stays empty");

    /* All 31 slots occupied: the compaction must not run off the end into the
     * display-status word that sits immediately behind MIDI_IN. */
    for (int i = 0; i < SHADOW_MIDI_IN_SLOTS; i++)
        put(buf, i, 0x09, 0x90, (uint8_t)(40 + i), 100, (uint8_t)i);
    clear_slot(buf, 0);
    CHECK(shadow_midi_in_compact(buf) == SHADOW_MIDI_IN_SLOTS - 1,
          "30 survivors out of a full frame");
    CHECK(move_sees(buf) == SHADOW_MIDI_IN_SLOTS - 1, "Move sees all 30");
    CHECK(shadow_midi_in_slot_empty(&buf[(SHADOW_MIDI_IN_SLOTS - 1) *
                                         SHADOW_MIDI_IN_STRIDE]),
          "the vacated last slot is zeroed, not left as a duplicate");
}

/* The timestamp is not part of the emptiness test — a slot carrying only a
 * stale timestamp is still a terminator, exactly as the SPI library reads it. */
static void test_empty_ignores_the_timestamp(void)
{
    uint8_t slot[SHADOW_MIDI_IN_STRIDE] = { 0, 0, 0, 0, 0xDE, 0xAD, 0xBE, 0xEF };
    CHECK(shadow_midi_in_slot_empty(slot),
          "a zero USB-MIDI payload is empty however stale the timestamp is");
    slot[0] = 0x09;
    CHECK(!shadow_midi_in_slot_empty(slot), "a nonzero header is an event");
}

static void test_geometry(void)
{
    CHECK(SHADOW_MIDI_IN_BYTES == 248,
          "MIDI_IN is 31 x 8 = 248 bytes, not MIDI_BUFFER_SIZE (256)");
}

int main(void)
{
    test_geometry();
    test_note_off_behind_a_filtered_knob();
    test_order_is_preserved_across_many_gaps();
    test_dense_frame_is_untouched();
    test_empty_and_full_frames();
    test_empty_ignores_the_timestamp();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS: MIDI_IN compaction — a filtered slot no longer hides the events behind it\n");
    return 0;
}
