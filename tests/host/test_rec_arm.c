/*
 * test_rec_arm — replays the MEASURED Record-LED sequence through the decoder.
 *
 * The frames below are not invented. They are the capture recorded in
 * docs/plans/2026-09-12-automation-lanes-design.md ("Measured: Move's Record
 * button", 2026-09-12, on hardware), transcribed one frame per row. A
 * synthetic sequence here would prove only that the decoder agrees with my
 * model of Move, which is the thing that was wrong about this button twice
 * over: first the CC number (118, from a stale header comment), then the
 * discriminator ("static means recording", which is also the resting state).
 *
 * Two assertions carry the whole decode and both are easy to lose:
 *
 *   1. The settled state changes ONLY at frame end. Move writes the base
 *      colour statically and *then* applies the animation, so one frame holds
 *      `static 127` followed by `blink`. Every frame here is fed message by
 *      message and the settled state is checked for stability after EACH one,
 *      so a decoder that acts per message is caught mid-frame rather than
 *      slipping through because the frame happens to settle correctly anyway.
 *
 *   2. Full brightness, not "non-zero". Frames A and F are static and non-zero
 *      (122, 124) and are the RESTING state.
 */
#include <stdio.h>
#include <string.h>
#include "rec_arm.h"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

/* One MIDI_OUT message, as the cable-0 scan hands it over. */
typedef struct { uint8_t status, d1, d2; } msg_t;

/* One SPI frame: the messages in it, and the state it must settle to. */
typedef struct {
    const char *name;
    msg_t msgs[4];
    int   n;
    int   want_recording;
} frame_t;

#define CC 86
#define B(ch) (uint8_t)(0xB0 | (ch))

static void run_frames(rec_arm_t *st, const frame_t *frames, int nframes)
{
    for (int f = 0; f < nframes; f++) {
        const frame_t *fr = &frames[f];
        const int before = st->recording;
        for (int m = 0; m < fr->n; m++) {
            rec_arm_on_led(st, fr->msgs[m].status, fr->msgs[m].d1, fr->msgs[m].d2);
            /* Rule 1, as an invariant rather than as an outcome. */
            CHECK(st->recording == before,
                  "%s: settled state moved %d -> %d mid-frame, on message %d "
                  "(a per-message decode reports RECORDING for one frame every "
                  "time a count-in begins)",
                  fr->name, before, st->recording, m);
        }
        const int got = rec_arm_frame_end(st);
        CHECK(got == fr->want_recording, "%s: recording=%d, want %d",
              fr->name, got, fr->want_recording);
        CHECK(st->recording == got, "%s: return value disagrees with the struct",
              fr->name);
    }
}

/* The captured arm sequence, one frame per row. Pulse counts from the capture
 * are in the names so a failure can be read against the log it came from. */
static void test_measured_sequence(void)
{
    printf("the measured Session-view arm sequence\n");
    rec_arm_t st;
    memset(&st, 0, sizeof(st));

    const frame_t frames[] = {
        /* A: resting. Static and NON-ZERO -- this is the frame that breaks a
         *    "static means recording" decode. */
        { "A pul=921 resting", { { B(0), CC, 122 } }, 1, 0 },
        /* B: armed, waiting. PULSE_HALF on channel 10; the static write that
         *    precedes it is the base colour going dark. */
        { "B pul=1043 armed", { { B(0), CC, 0 }, { B(10), CC, 127 } }, 2, 0 },
        /* C: queued, counting in. BLINK_4TH on channel 14, and the static 127
         *    ahead of it in the SAME frame is the trap. */
        { "C pul=1146 counting in", { { B(0), CC, 127 }, { B(14), CC, 0 } }, 2, 0 },
        /* D: recording. Static, full brightness, nothing after it. */
        { "D pul=1154 RECORDING", { { B(0), CC, 127 } }, 1, 1 },
        /* E: stopped with Play; Record stays armed, so back to the pulse. */
        { "E pul=1471 armed again", { { B(0), CC, 0 }, { B(10), CC, 127 } }, 2, 0 },
        /* F: disarmed. Two static writes, both non-zero, both resting. */
        { "F pul=1471 disarmed", { { B(0), CC, 122 }, { B(0), CC, 124 } }, 2, 0 },
    };
    run_frames(&st, frames, (int)(sizeof(frames) / sizeof(frames[0])));
}

/* Move emits an LED packet only when that LED changes, so most frames carry
 * no CC 86 at all. Absence must mean "unchanged", never "off": in set
 * selection Record does nothing and lights nothing, and a frame-end that
 * cleared on absence would drop a recording every frame. */
static void test_absence_is_unchanged(void)
{
    printf("a frame with no CC 86 leaves the state alone\n");
    rec_arm_t st;
    memset(&st, 0, sizeof(st));

    rec_arm_on_led(&st, B(0), CC, 127);
    CHECK(rec_arm_frame_end(&st) == 1, "did not reach RECORDING");

    for (int i = 0; i < 200; i++) {
        /* Traffic that is not this button: a pad LED, a step LED, a CC on
         * another control. */
        rec_arm_on_led(&st, 0x90, 93, 21);
        rec_arm_on_led(&st, 0x90, 20, 126);
        rec_arm_on_led(&st, B(0), 88, 127);
        CHECK(rec_arm_frame_end(&st) == 1,
              "frame %d with no CC 86 cleared the recording state", i);
    }

    /* And it is still the same button that turns it off. */
    rec_arm_on_led(&st, B(0), CC, 0);
    CHECK(rec_arm_frame_end(&st) == 0, "a dark static write did not clear it");
}

/* CC 118 is the same physical button as Sample per schwung-spi's header, and
 * it never appeared in the arm sequence. Nothing but 86 may move this state. */
static void test_other_cc_numbers_ignored(void)
{
    printf("only CC 86 is the Record LED\n");
    rec_arm_t st;
    memset(&st, 0, sizeof(st));

    const uint8_t others[] = { 118, 85, 87, 88, 0, 127 };
    for (unsigned i = 0; i < sizeof(others); i++) {
        CHECK(rec_arm_on_led(&st, B(0), others[i], 127) == 0,
              "CC %d was taken as the Record LED", others[i]);
        CHECK(rec_arm_frame_end(&st) == 0, "CC %d set RECORDING", others[i]);
    }

    /* A note-on carrying 86 as a NOTE number is not a CC. */
    CHECK(rec_arm_on_led(&st, 0x90, CC, 127) == 0, "note 86 was taken as CC 86");
    CHECK(rec_arm_frame_end(&st) == 0, "note 86 set RECORDING");
}

/* Every animation channel means flashing, whatever colour it animates to --
 * no rate measurement, no hue comparison. 127 on an animation channel is the
 * case that matters: it is full brightness and it is NOT recording. */
static void test_every_animation_channel_flashes(void)
{
    printf("channels 0x06..0x0F never read as recording\n");
    for (int ch = 0x06; ch <= 0x0F; ch++) {
        rec_arm_t st;
        memset(&st, 0, sizeof(st));
        rec_arm_on_led(&st, B(0), CC, 127);
        CHECK(rec_arm_frame_end(&st) == 1, "ch %d: setup failed", ch);
        rec_arm_on_led(&st, B((uint8_t)ch), CC, 127);
        CHECK(rec_arm_frame_end(&st) == 0,
              "channel 0x%02X d2=127 read as RECORDING", ch);
        CHECK(st.flashing == 1, "channel 0x%02X did not report flashing", ch);
    }
}

/* The brightness rule, swept. Only 127 records. */
static void test_only_full_brightness_records(void)
{
    printf("static: only d2=127 records\n");
    for (int v = 0; v <= 127; v++) {
        rec_arm_t st;
        memset(&st, 0, sizeof(st));
        rec_arm_on_led(&st, B(0), CC, (uint8_t)v);
        const int got = rec_arm_frame_end(&st);
        CHECK(got == (v == 127), "static d2=%d -> recording=%d, want %d",
              v, got, v == 127);
    }
}

/* `seen` exists so a readout can tell "disarmed" from "this button has never
 * reported anything", which are the same zero and have different causes. */
static void test_seen_flag(void)
{
    printf("seen distinguishes 'off' from 'never reported'\n");
    rec_arm_t st;
    memset(&st, 0, sizeof(st));
    CHECK(st.seen == 0, "seen set before any event");
    rec_arm_on_led(&st, 0x90, 93, 21);
    rec_arm_frame_end(&st);
    CHECK(st.seen == 0, "unrelated traffic set seen");
    rec_arm_on_led(&st, B(0), CC, 122);
    rec_arm_frame_end(&st);
    CHECK(st.seen == 1, "a CC 86 did not set seen");
}

int main(void)
{
    test_measured_sequence();
    test_absence_is_unchanged();
    test_other_cc_numbers_ignored();
    test_every_animation_channel_flashes();
    test_only_full_brightness_records();
    test_seen_flag();

    if (failures) { printf("FAILED: %d\n", failures); return 1; }
    printf("PASS: rec_arm\n");
    return 0;
}
