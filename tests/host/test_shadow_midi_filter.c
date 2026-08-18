/* Regression test: stale MIDI_IN slots must not reach the shadow UI ring.
 *
 * Background: the post-ioctl scan walks the UNFILTERED hardware MIDI_IN
 * buffer, which is never cleared wholesale — consumed slots keep their bytes
 * and are re-read every SPI frame (~44/s).  In overtake mode the scan widens
 * to accept SysEx CINs (0x04-0x07), and the existing sub-0x80 guard in
 * shadow_ui_midi_publish() deliberately exempts exactly that range.  A stale
 * slot whose CIN nibble lands in 0x04-0x07 with a zeroed payload therefore
 * sailed through and was dispatched into JS as `status=0 d1=0 d2=0`,
 * observed at ~10/s (91,056 events in one 2.5 h log).
 */
#include <stdio.h>

#include "shadow_midi_filter.h"

static int fails = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails++; } \
} while (0)

static void test_empty_slot_dropped(void) {
    CHECK(!shadow_midi_forwardable(0x00, 0x00, 0x00, 0x00),
          "an entirely empty slot must be dropped");
}

static void test_voice_messages(void) {
    CHECK(shadow_midi_forwardable(0x09, 0x90, 68, 100),
          "cable-0 note-on must be forwarded");
    CHECK(shadow_midi_forwardable(0x2B, 0xB0, 14, 1),
          "cable-2 CC must be forwarded");
    /* The torn/stale case the 2026-06-18 guard already covered. */
    CHECK(!shadow_midi_forwardable(0x09, 0x00, 0x00, 0x00),
          "voice CIN with sub-0x80 status is stale and must be dropped");
    CHECK(!shadow_midi_forwardable(0x0B, 0x40, 14, 1),
          "voice CIN with a data byte in the status slot must be dropped");
}

static void test_sysex_zero_payload_dropped(void) {
    /* THE BUG: these are the events that flooded the overtake tools. */
    CHECK(!shadow_midi_forwardable(0x04, 0x00, 0x00, 0x00),
          "SysEx start CIN with all-zero payload is a stale slot");
    CHECK(!shadow_midi_forwardable(0x05, 0x00, 0x00, 0x00),
          "SysEx 1-byte-end CIN with all-zero payload is a stale slot");
    CHECK(!shadow_midi_forwardable(0x06, 0x00, 0x00, 0x00),
          "SysEx 2-byte-end CIN with all-zero payload is a stale slot");
    CHECK(!shadow_midi_forwardable(0x07, 0x00, 0x00, 0x00),
          "SysEx 3-byte-end CIN with all-zero payload is a stale slot");
}

static void test_real_sysex_still_forwarded(void) {
    /* Must not regress the reason SysEx was exempted in the first place:
     * genuine SysEx payload bytes are legitimately < 0x80. */
    CHECK(shadow_midi_forwardable(0x04, 0xF0, 0x00, 0x21),
          "SysEx start (F0 00 21) must be forwarded");
    CHECK(shadow_midi_forwardable(0x04, 0x00, 0x21, 0x1D),
          "mid-SysEx continuation with a nonzero data byte must be forwarded");
    CHECK(shadow_midi_forwardable(0x05, 0xF7, 0x00, 0x00),
          "single-byte SysEx end (F7) must be forwarded");
    CHECK(shadow_midi_forwardable(0x07, 0x12, 0x02, 0xF7),
          "3-byte SysEx end must be forwarded");
}

static void test_non_packet_cins_dropped(void) {
    CHECK(!shadow_midi_forwardable(0x01, 0x90, 68, 100),
          "CIN 0x01 is not a forwardable packet type");
    CHECK(!shadow_midi_forwardable(0x03, 0x90, 68, 100),
          "CIN 0x03 is not a forwardable packet type");
}

int main(void) {
    test_empty_slot_dropped();
    test_voice_messages();
    test_sysex_zero_payload_dropped();
    test_real_sysex_still_forwarded();
    test_non_packet_cins_dropped();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("test_shadow_midi_filter: all checks passed\n");
    return 0;
}
