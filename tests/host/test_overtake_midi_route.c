#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

    CHECK(shadow_overtake_midi_drain(&shared, 1, midi_in, 31) == 2,
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
    CHECK(shadow_overtake_midi_drain(&shared, 0, midi_in, 31) == 1,
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
        packet[2] = (uint8_t)i;
        CHECK(shadow_overtake_midi_send(packet, 4) == 4,
              "dedicated ring accepts every available slot");
    }
    packet[2] = 127;
    CHECK(shadow_overtake_midi_send(packet, 4) == 0,
          "dedicated ring rejects a packet when full");

    memset(midi_in, 0, sizeof(midi_in));
    CHECK(shadow_overtake_midi_drain(NULL, 1, midi_in, 1) == 1,
          "bounded drain copies only the requested packet count");
    CHECK(midi_in[2] == 0,
          "ring overflow does not disturb the oldest packet");
}

int main(void)
{
    test_active_overtake_uses_only_dedicated_queue();
    test_full_queue_preserves_fifo();

    if (fails) {
        fprintf(stderr, "%d check(s) failed\n", fails);
        return 1;
    }
    printf("PASS: overtake DSP MIDI reaches Move without consuming the shared test bus\n");
    return 0;
}
