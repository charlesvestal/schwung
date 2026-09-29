/*
 * test_surface_live.c -- the scanners behind the web mirror's device view.
 *
 * The SysEx fixture is REAL: Move's own RGB LED writes, copied from the
 * cable-0 MIDI_OUT trace in docs/superpowers/specs/2026-08-18-usbc-out-source-
 * capture.txt (frame f5), including a message that frame cuts in half.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "surface_live_shm.h"

static surface_live_shm_t S;
static surface_live_writer_t W;
static int fails = 0;

#define CHECK(cond, ...) do { if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static void out_frame(const uint8_t (*pk)[4], int n) {
    uint8_t buf[80] = {0};
    assert(n <= 20);
    for (int i = 0; i < n; i++) memcpy(buf + i * 4, pk[i], 4);
    surface_live_scan_out(&S, &W, buf, sizeof buf);
}

static void in_frame(const uint8_t (*pk)[4], int n) {
    uint8_t buf[248] = {0};
    assert(n <= 31);
    for (int i = 0; i < n; i++) {
        memcpy(buf + i * 8, pk[i], 4);
        buf[i * 8 + 4] = 0xAA;              /* timestamp bytes: must be ignored */
        buf[i * 8 + 5] = 0x90;
    }
    surface_live_scan_in(&S, &W, buf, sizeof buf);
}

static void test_note_and_cc_leds(void) {
    surface_live_init(&S, &W);
    CHECK(S.note_led_anim[68] == SURFACE_LIVE_ANIM_NONE, "unwritten LED must say so");
    uint32_t seq0 = S.seq;
    const uint8_t f[][4] = {
        { 0x09, 0x90, 68, 5 },      /* pad 1, colour 5, solid */
        { 0x09, 0x9E, 16, 126 },    /* step 1, blink channel 14 */
        { 0x0B, 0xB0, 49, 127 },    /* Shift white LED */
        { 0x2B, 0xB0, 50, 99 },     /* cable 2: an external device, NOT the surface */
    };
    out_frame(f, 4);
    CHECK(S.note_led[68] == 5 && S.note_led_anim[68] == 0, "pad LED");
    CHECK(S.note_led[16] == 126 && S.note_led_anim[16] == 14, "step LED keeps its animation");
    CHECK(S.cc_led[49] == 127 && S.cc_led_anim[49] == 0, "cc LED");
    CHECK(S.cc_led[50] == 0 && S.cc_led_anim[50] == SURFACE_LIVE_ANIM_NONE, "cable 2 ignored");
    CHECK(S.seq == seq0 + 2 && (S.seq & 1) == 0, "one even bump per changed frame, got %u", S.seq - seq0);

    /* Move turns a pad off with a note-off */
    const uint8_t off[][4] = { { 0x08, 0x80, 68, 64 } };
    out_frame(off, 1);
    CHECK(S.note_led[68] == 0, "note-off clears the LED");

    /* nothing changed: the page must not be touched at all */
    uint32_t seq1 = S.seq;
    out_frame(off, 1);
    const uint8_t none[][4] = { { 0, 0, 0, 0 } };
    out_frame(none, 1);
    CHECK(S.seq == seq1, "an unchanged frame bumped seq");
}

static void test_real_rgb_sysex(void) {
    surface_live_init(&S, &W);
    /* f5: one complete message for idx 0x28 ... */
    const uint8_t a[][4] = {
        { 0x04, 0xf0, 0x00, 0x21 }, { 0x04, 0x1d, 0x01, 0x01 }, { 0x04, 0x3b, 0x10, 0x28 },
        { 0x04, 0x62, 0x00, 0x7f }, { 0x04, 0x01, 0x54, 0x00 }, { 0x05, 0xf7, 0x00, 0x00 },
        /* ... and the start of idx 0x2a, which the frame cuts off */
        { 0x04, 0xf0, 0x00, 0x21 }, { 0x04, 0x1d, 0x01, 0x01 }, { 0x04, 0x3b, 0x10, 0x2a },
    };
    out_frame(a, 9);
    CHECK(S.rgb[1][0x28][3] == 1, "idx 0x28 decoded");
    CHECK(S.rgb[1][0x28][0] == 98 && S.rgb[1][0x28][1] == 255 && S.rgb[1][0x28][2] == 84,
          "rgb(0x28) = %d,%d,%d, want 98,255,84", S.rgb[1][0x28][0], S.rgb[1][0x28][1], S.rgb[1][0x28][2]);
    CHECK(S.rgb[1][0x2a][3] == 0, "a half-received message must not land");
    /* the next frame carries the tail (payload from f5's idx 0x29 message) */
    const uint8_t b[][4] = { { 0x04, 0x2c, 0x01, 0x1e }, { 0x04, 0x00, 0x00, 0x00 }, { 0x05, 0xf7, 0x00, 0x00 } };
    out_frame(b, 3);
    CHECK(S.rgb[1][0x2a][3] == 1 && S.rgb[1][0x2a][0] == 172 && S.rgb[1][0x2a][1] == 30 && S.rgb[1][0x2a][2] == 0,
          "a message split across frames: %d,%d,%d valid=%d",
          S.rgb[1][0x2a][0], S.rgb[1][0x2a][1], S.rgb[1][0x2a][2], S.rgb[1][0x2a][3]);

    /* latest wins: a palette write to the same CC after the RGB repaints it,
     * and an RGB after the palette write takes it back */
    const uint8_t pal[][4] = { { 0x0B, 0xB0, 0x28, 122 } };
    out_frame(pal, 1);
    CHECK(S.rgb[1][0x28][3] == 0 && S.cc_led[0x28] == 122, "a later palette write must win");
    out_frame(a, 6);
    CHECK(S.rgb[1][0x28][3] == 1, "a later RGB write must win");

    /* ch 0 addresses a NOTE: the same idx lands in the other table */
    surface_live_init(&S, &W);
    const uint8_t c[][4] = {
        { 0x04, 0xf0, 0x00, 0x21 }, { 0x04, 0x1d, 0x01, 0x01 }, { 0x04, 0x3b, 0x00, 0x28 },
        { 0x04, 0x62, 0x00, 0x7f }, { 0x04, 0x01, 0x54, 0x00 }, { 0x05, 0xf7, 0x00, 0x00 },
    };
    out_frame(c, 6);
    CHECK(S.rgb[1][0x28][3] == 0, "a ch-0 (note) write landed in the CC table");
    CHECK(S.rgb[0][0x28][3] == 1 && S.rgb[0][0x28][1] == 255, "ch 0 is a note-addressed RGB write");

    /* any other channel byte is not an LED write */
    surface_live_init(&S, &W);
    const uint8_t d[][4] = {
        { 0x04, 0xf0, 0x00, 0x21 }, { 0x04, 0x1d, 0x01, 0x01 }, { 0x04, 0x3b, 0x20, 0x28 },
        { 0x04, 0x62, 0x00, 0x7f }, { 0x04, 0x01, 0x54, 0x00 }, { 0x05, 0xf7, 0x00, 0x00 },
    };
    out_frame(d, 6);
    CHECK(!S.rgb[0][0x28][3] && !S.rgb[1][0x28][3], "ch 2 decoded");
    CHECK((S.seq & 1) == 0, "seq left odd");
}

static void test_presses(void) {
    surface_live_init(&S, &W);
    const uint8_t f[][4] = {
        { 0x09, 0x90, 70, 100 },    /* pad down */
        { 0x0A, 0xA0, 70, 40 },     /* pressure */
        { 0x0B, 0xB0, 49, 127 },    /* Shift down */
        { 0x0B, 0xB0, 71, 3 },      /* knob 1 +3 */
        { 0x0B, 0xB0, 71, 126 },    /* knob 1 -2 */
        { 0x0B, 0xB0, 14, 1 },      /* jog +1 */
        { 0x2B, 0xB0, 49, 0 },      /* cable 2 */
    };
    in_frame(f, 7);
    CHECK(S.frame == 1, "frame counter");
    CHECK(S.note_down[70] == 100 && S.note_pressure[70] == 40, "pad held with pressure");
    CHECK(S.cc_value[49] == 127, "Shift held (cable 2 must not release it)");
    CHECK(S.enc_pos[0] == 1, "knob 1 net +1, got %d", S.enc_pos[0]);
    CHECK(S.enc_pos[9] == 1, "jog");
    CHECK(S.event_count == 2, "turns and pressure are state, not events: %u", S.event_count);
    CHECK(S.events[0].d1 == 70 && S.events[1].d1 == 49 && S.events[1].frame == 1, "ring order");

    const uint8_t g[][4] = { { 0x09, 0x90, 70, 0 } };   /* velocity-0 note-on = up */
    in_frame(g, 1);
    CHECK(S.note_down[70] == 0 && S.note_pressure[70] == 0, "release clears pressure");

    for (int i = 0; i < 40; i++) {
        const uint8_t h[][4] = { { 0x0B, 0xB0, 49, (uint8_t)((i & 1) ? 0 : 127) } };
        in_frame(h, 1);
    }
    CHECK(S.event_count == 43, "count");
    CHECK(S.events[42 % SURFACE_LIVE_EVENTS].d2 == 0 && S.events[41 % SURFACE_LIVE_EVENTS].d2 == 127 && S.events[42 % SURFACE_LIVE_EVENTS].frame == 42,
          "ring wraps to the newest");
    CHECK((S.seq & 1) == 0, "seq left odd");
}

int main(void) {
    test_note_and_cc_leds();
    test_real_rgb_sysex();
    test_presses();
    if (fails) { printf("test_surface_live: %d FAILED\n", fails); return 1; }
    printf("test_surface_live: all passed\n");
    return 0;
}
