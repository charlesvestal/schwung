/* Beat boundaries and the click voice, run natively.
 *
 * The boundary cases here are the ones that are silent when wrong: an
 * off-by-one accent, and the transport reset that eats the first click of a
 * take. Both feel like "the metronome is a bit off" rather than a bug, which
 * is exactly why they have to be pinned somewhere they can actually run — the
 * caller lives in the shim, which cannot be built on the dev machine.
 */
#include <stdio.h>
#include <math.h>
#include "metronome_click.h"

static int failures = 0;

static void expect_beat(int prev, int now, int bpb, int want, const char *what)
{
    int got = metronome_beat_crossed(prev, now, bpb);
    if (got != want) {
        printf("FAIL: %s: crossed(%d,%d,bpb=%d) = %d, want %d\n",
               what, prev, now, bpb, got, want);
        failures++;
    }
}

int main(void)
{
    /* ---- no prior position ---- */
    expect_beat(-1, 0, 4, -1, "no prior pulse is not a crossing, even at pulse 0");
    expect_beat(-1, 97, 4, -1, "no prior pulse is not a crossing, even mid-bar");

    /* ---- 4/4, on the CORRECTED grid ----
     *
     * The downbeat is pulse 1, not pulse 0: shadow_transport_pulses is zeroed
     * on MIDI Start and the first clock increments it, and that first clock IS
     * the downbeat. Measured on hardware 2026-09-01 — firing at 24N put the
     * click 144.3 ms early at 20 BPM and 40.4 ms at 120 BPM, which solves to
     * exactly one pulse of phase plus a constant 19.6 ms of Link Audio transit.
     */
    expect_beat(0, 1, 4, 0, "pulse 1 is the downbeat");
    expect_beat(24, 25, 4, 1, "pulse 25 is beat 1");
    expect_beat(48, 49, 4, 2, "pulse 49 is beat 2");
    expect_beat(72, 73, 4, 3, "pulse 73 is beat 3");
    expect_beat(96, 97, 4, 0, "pulse 97 is the downbeat of bar 2");
    expect_beat(25, 25, 4, -1, "no advance is no crossing");
    expect_beat(1, 2, 4, -1, "inside a beat");

    /* The OLD, WRONG boundaries must now be silent. These are the exact
     * assertions that were green while the click was a whole pulse early, so
     * they are inverted rather than deleted — a regression here would sound
     * like "the metronome is not on the beat" and nothing else. */
    expect_beat(23, 24, 4, -1, "pulse 24 is NOT a beat (the old off-by-one)");
    expect_beat(95, 96, 4, -1, "pulse 96 is NOT the downbeat (the old off-by-one)");

    /* ---- transport reset: MIDI Start zeroes the counter ---- */
    expect_beat(95, 0, 4, -1, "a backwards count is a RESTART, and the downbeat has not arrived");
    expect_beat(0, 1, 4, 0, "it arrives on the next pulse");

    /* ---- 3/4 ---- */
    expect_beat(72, 73, 3, 0, "3/4 accents pulse 73");
    expect_beat(96, 97, 3, 1, "3/4 does NOT accent pulse 97");

    /* ---- degenerate beats_per_bar clamps rather than dividing by zero ---- */
    expect_beat(96, 97, 0, 0, "bpb 0 clamps to 4");
    expect_beat(96, 97, -3, 0, "negative bpb clamps to 4");

    /* ---- several boundaries in one call report the latest, once ---- */
    expect_beat(0, 97, 4, 0, "a wide span reports the latest boundary");
    expect_beat(0, 51, 4, 2, "a two-beat span reports beat 2");

    /* ---- the pending-click countdown ---- */
    {
        int p = -1;
        if (metronome_pending_advance(&p, 128) != -1) {
            printf("FAIL: nothing pending must not fire\n"); failures++;
        }
        p = 0;
        if (metronome_pending_advance(&p, 128) != 0 || p != -1) {
            printf("FAIL: a zero countdown fires at offset 0 and clears\n"); failures++;
        }
        p = 127;
        if (metronome_pending_advance(&p, 128) != 127 || p != -1) {
            printf("FAIL: the last frame of a block still fires in it\n"); failures++;
        }
        p = 128;
        if (metronome_pending_advance(&p, 128) != -1 || p != 0) {
            printf("FAIL: exactly one block away fires NEXT block, at offset 0; got p=%d\n", p);
            failures++;
        }
        /* The real case: 700 frames of Link Audio compensation across
         * 128-frame blocks lands at 5*128 + 60 = 700. */
        p = 700;
        int fired_at = -1, blocks = 0;
        for (int i = 0; i < 20; i++) {
            int off = metronome_pending_advance(&p, 128);
            if (off >= 0) { fired_at = off; blocks = i; break; }
        }
        if (blocks != 5 || fired_at != 60) {
            printf("FAIL: a 700-frame delay must fire in block 5 at offset 60, got block %d offset %d\n",
                   blocks, fired_at);
            failures++;
        }
    }

    /* ---- voice ---- */
    {
        metronome_voice_t v = {0};
        if (metronome_voice_next(&v) != 0.0f) {
            printf("FAIL: an untriggered voice must return exactly 0.0f\n");
            failures++;
        }
        metronome_voice_trigger(&v, METRONOME_FREQ_BEAT_HZ, 1.0f,
                                METRONOME_DECAY_SECONDS, 44100.0f);
        if (!metronome_voice_active(&v)) {
            printf("FAIL: a triggered voice must be active\n");
            failures++;
        }
        float peak = 0.0f, prev_env = 2.0f;
        int went_silent = 0;
        for (int i = 0; i < 44100; i++) {
            float s = metronome_voice_next(&v);
            if (fabsf(s) > peak) peak = fabsf(s);
            if (s > 1.0f || s < -1.0f) {
                printf("FAIL: voice left -1..1 at sample %d (%f)\n", i, s);
                failures++;
                break;
            }
            /* Envelope must not grow. */
            if (v.amp > prev_env + 1e-6f) {
                printf("FAIL: envelope grew at sample %d\n", i);
                failures++;
                break;
            }
            prev_env = v.amp;
            if (!metronome_voice_active(&v)) { went_silent = 1; break; }
        }
        if (peak < 0.5f) {
            printf("FAIL: voice peak %f is implausibly quiet\n", peak);
            failures++;
        }
        if (!went_silent) {
            printf("FAIL: voice never decayed to silence within 1 s\n");
            failures++;
        }
        if (metronome_voice_next(&v) != 0.0f) {
            printf("FAIL: a decayed voice must return exactly 0.0f\n");
            failures++;
        }
    }

    if (failures) { printf("test_metronome_click: FAIL (%d)\n", failures); return 1; }
    printf("test_metronome_click: PASS\n");
    return 0;
}
