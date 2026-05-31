// SPDX-License-Identifier: MIT
//
// Phase 5b smoke test: push a few MIDI messages into the sim queue, drain to
// a mock mailbox, verify the encoded 8-byte AblSpiMidiEvent slots. No audio.

#include <stdio.h>
#include <string.h>

#include "host/sim_backend.h"
#include "schwung_spi_lib.h"

static int fail_count = 0;
#define EXPECT_EQ(a, b, msg) do { \
    if ((int)(a) != (int)(b)) { \
        fprintf(stderr, "FAIL: %s — got %d expected %d (line %d)\n", \
                (msg), (int)(a), (int)(b), __LINE__); fail_count++; \
    } } while (0)

int main(void) {
    uint8_t mailbox[SCHWUNG_PAGE_SIZE];
    memset(mailbox, 0, sizeof(mailbox));

    // Push 3 MIDI messages: note-on, CC, note-off.
    uint8_t m1[] = {0x9F, 60, 100};   // ch16 note-on note 60 vel 100
    uint8_t m2[] = {0xBF, 71, 64};    // ch16 CC 71 = encoder 0 turn
    uint8_t m3[] = {0x8F, 60, 0};     // ch16 note-off note 60

    EXPECT_EQ(schwung_sim_push_midi_in(m1, 3), 0, "push m1");
    EXPECT_EQ(schwung_sim_push_midi_in(m2, 3), 0, "push m2");
    EXPECT_EQ(schwung_sim_push_midi_in(m3, 3), 0, "push m3");

    // Drain into mailbox.
    int n = schwung_sim_drain_midi_in_to_mailbox(mailbox);
    EXPECT_EQ(n, 3, "drain count");

    // Inspect slot 0: USB-MIDI [cin=9, status=0x9F, 60, 100] + timestamp 1.
    const uint8_t *s0 = mailbox + SCHWUNG_OFF_IN_MIDI;
    EXPECT_EQ(s0[0], 0x09, "slot0 cin/cable");   // cable 0, cin 9 (note-on)
    EXPECT_EQ(s0[1], 0x9F, "slot0 status");
    EXPECT_EQ(s0[2], 60,   "slot0 d1");
    EXPECT_EQ(s0[3], 100,  "slot0 d2");
    EXPECT_EQ(s0[4], 1,    "slot0 ts byte0");    // ts=1 little-endian
    EXPECT_EQ(s0[5], 0,    "slot0 ts byte1");

    // Slot 1: CC.
    const uint8_t *s1 = mailbox + SCHWUNG_OFF_IN_MIDI + 8;
    EXPECT_EQ(s1[0], 0x0B, "slot1 cin/cable");   // cin 0xB = CC
    EXPECT_EQ(s1[1], 0xBF, "slot1 status");
    EXPECT_EQ(s1[2], 71,   "slot1 d1");
    EXPECT_EQ(s1[3], 64,   "slot1 d2");
    EXPECT_EQ(s1[4], 2,    "slot1 ts");          // monotonic increment

    // Slot 2: note-off.
    const uint8_t *s2 = mailbox + SCHWUNG_OFF_IN_MIDI + 16;
    EXPECT_EQ(s2[0], 0x08, "slot2 cin/cable");   // cin 0x8 = note-off
    EXPECT_EQ(s2[1], 0x8F, "slot2 status");
    EXPECT_EQ(s2[2], 60,   "slot2 d1");
    EXPECT_EQ(s2[3], 0,    "slot2 d2");
    EXPECT_EQ(s2[4], 3,    "slot2 ts");

    // Slot 3 should be all zeros (drain zero-fills unused usable slots).
    const uint8_t *s3 = mailbox + SCHWUNG_OFF_IN_MIDI + 24;
    int s3_all_zero = 1;
    for (int i = 0; i < 8; i++) if (s3[i]) { s3_all_zero = 0; break; }
    EXPECT_EQ(s3_all_zero, 1, "slot3 zeroed (terminator)");

    // Second drain with no new messages: should produce 0.
    EXPECT_EQ(schwung_sim_drain_midi_in_to_mailbox(mailbox), 0, "drain empty");

    if (fail_count == 0) { printf("PASS\n"); return 0; }
    fprintf(stderr, "FAIL: %d assertion(s) failed\n", fail_count);
    return 1;
}
