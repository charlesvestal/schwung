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
    expect_beat(-1, 96, 4, -1, "no prior pulse is not a crossing, even mid-bar");

    /* ---- 4/4 ---- */
    expect_beat(0, 1, 4, -1, "inside a beat");
    expect_beat(23, 24, 4, 1, "pulse 24 is beat 1");
    expect_beat(47, 48, 4, 2, "pulse 48 is beat 2");
    expect_beat(71, 72, 4, 3, "pulse 72 is beat 3");
    expect_beat(95, 96, 4, 0, "pulse 96 is the downbeat of bar 2");
    expect_beat(24, 24, 4, -1, "no advance is no crossing");

    /* ---- transport reset: MIDI Start zeroes the counter ---- */
    expect_beat(95, 0, 4, 0, "a backwards count is a downbeat, not a miss");

    /* ---- 3/4 ---- */
    expect_beat(71, 72, 3, 0, "3/4 accents pulse 72");
    expect_beat(95, 96, 3, 1, "3/4 does NOT accent pulse 96");

    /* ---- degenerate beats_per_bar clamps rather than dividing by zero ---- */
    expect_beat(95, 96, 0, 0, "bpb 0 clamps to 4");
    expect_beat(95, 96, -3, 0, "negative bpb clamps to 4");

    /* ---- several boundaries in one call report the latest, once ---- */
    expect_beat(0, 96, 4, 0, "a wide span reports the latest boundary");
    expect_beat(0, 50, 4, 2, "a two-beat span reports beat 2");

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
