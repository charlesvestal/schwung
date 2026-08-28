/*
 * Can shadow_midi_out_t.write_idx address its own buffer?
 *
 * It could not. The buffer is 512 bytes and write_idx was a uint8_t, so:
 *
 *   - only the first 63 packets were reachable; the back half of the buffer
 *     was never written and never read;
 *   - `write_idx = write_offset + 4` wrapped 252 -> 0, silently rewinding the
 *     buffer mid-flush so later packets overwrote earlier ones;
 *   - the `write_offset + 4 <= SHADOW_MIDI_OUT_BUFFER_SIZE` bounds check in
 *     js_shadow_midi_send could never fire, because a uint8_t cannot reach
 *     512. It read as a working overflow check and was dead code.
 *
 * Symptom on hardware: an overtake module playing heavily loses LED updates,
 * permanently, because input_filter's setLED caches the colour it believes it
 * sent and suppresses the next identical repaint.
 *
 * This is the "~64 packets, >60/frame overflows" limit CLAUDE.md recorded as a
 * property of the buffer. It was a property of the FIELD.
 *
 * The test drives the real write loop against the real struct, so it fails if
 * the field is ever narrowed back — which is the only way this returns.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "shadow_constants.h"

static int failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

/* Byte-for-byte the write step of js_shadow_midi_send. Returns 1 if queued. */
static int push_packet(shadow_midi_out_t *m, const uint8_t pkt[4]) {
    int write_offset = m->write_idx;
    if (write_offset + 4 <= SHADOW_MIDI_OUT_BUFFER_SIZE) {
        memcpy(&m->buffer[write_offset], pkt, 4);
        m->write_idx = (uint16_t)(write_offset + 4);
        return 1;
    }
    return 0;
}

int main(void) {
    static shadow_midi_out_t m;
    const int CAPACITY = SHADOW_MIDI_OUT_BUFFER_SIZE / 4;

    /* 1. THE WHOLE BUFFER IS REACHABLE. */
    printf("every packet the buffer has room for is accepted\n");
    memset(&m, 0, sizeof(m));
    int queued = 0;
    for (int i = 0; i < CAPACITY; i++) {
        uint8_t pkt[4] = { 0x09, 0x90, (uint8_t)i, 100 };
        queued += push_packet(&m, pkt);
    }
    check(queued == CAPACITY, "all 128 packets queued, not 63");
    check(m.write_idx == SHADOW_MIDI_OUT_BUFFER_SIZE,
          "write_idx reaches the end of the buffer");

    /* 2. NOTHING REWOUND. The wrap overwrote packet 0 with packet 64; the
     * note number in each slot is what proves it did not happen here. */
    printf("no packet was overwritten by a later one\n");
    int intact = 1;
    for (int i = 0; i < CAPACITY; i++)
        if (m.buffer[i * 4 + 2] != (uint8_t)i) { intact = 0; break; }
    check(intact, "every slot still holds the packet written to it");

    /* 3. THE GUARD ACTUALLY FIRES. It could not before: a uint8_t
     * write_offset cannot reach 512, so the bounds check was unreachable and
     * a full buffer was indistinguishable from a successful write. */
    printf("a full buffer refuses, and says so\n");
    uint8_t extra[4] = { 0x09, 0x90, 127, 100 };
    check(push_packet(&m, extra) == 0, "the packet past the end is refused");
    check(m.write_idx == SHADOW_MIDI_OUT_BUFFER_SIZE,
          "and a refused write does not advance the cursor");

    /* 4. THE SHIM READS WHAT WAS WRITTEN. shadow_inject_ui_midi_out does
     * `int snapshot_len = midi_out_shm->write_idx`, so a field that cannot
     * express the length silently truncates the drain as well as the fill. */
    printf("the length the shim reads covers everything written\n");
    int snapshot_len = m.write_idx;
    check(snapshot_len == SHADOW_MIDI_OUT_BUFFER_SIZE,
          "the drain sees all 512 bytes, not the low 8 bits of the count");

    /* 5. THE STRUCT DID NOT GROW. Both processes map by sizeof and are built
     * together, but a size change is still an ABI change and this one was
     * meant to be free — it takes a reserved byte. */
    printf("widening the field cost no memory\n");
    check(sizeof(shadow_midi_out_t) == 4 + SHADOW_MIDI_OUT_BUFFER_SIZE,
          "sizeof is unchanged at header + buffer");

    if (failures) {
        printf("FAILURES: %d\n", failures);
        return 1;
    }
    printf("PASS: shadow MIDI out buffer is fully addressable\n");
    return 0;
}
