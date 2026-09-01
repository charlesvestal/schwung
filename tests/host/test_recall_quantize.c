/*
 * The arithmetic behind a quantized snapshot recall.
 *
 * Boundary maths is wrong SILENTLY — an off-by-one division fires a beat early
 * forever and reads as bad timing rather than as a bug — so it lives in a
 * header this can run natively instead of inside schwung_shim.c, which cannot
 * be built on the dev machine.
 */
#include <assert.h>
#include <stdio.h>
#include "recall_quantize.h"

int main(void) {
    const int BEAT = 24, BAR = 96, TWO_BAR = 192;

    /* ---- the NEXT boundary, never the current one -------------------- */

    /* Mid-beat: the following beat. */
    assert(recall_next_boundary(5, BEAT) == 24);
    assert(recall_next_boundary(23, BEAT) == 24);

    /*
     * EXACTLY on the boundary still means the NEXT one. Pressing on the
     * downbeat must not fire instantly — that is indistinguishable from
     * quantize being off, which is the one outcome that makes the feature look
     * broken rather than merely early or late.
     */
    assert(recall_next_boundary(0, BEAT) == 24);
    assert(recall_next_boundary(24, BEAT) == 48);
    assert(recall_next_boundary(96, BAR) == 192);

    /* Bars and two-bars land on their own grid, not on beats. */
    assert(recall_next_boundary(1, BAR) == 96);
    assert(recall_next_boundary(95, BAR) == 96);
    assert(recall_next_boundary(97, BAR) == 192);
    assert(recall_next_boundary(1, TWO_BAR) == 192);
    assert(recall_next_boundary(191, TWO_BAR) == 192);

    /* Off, and defensive inputs. */
    assert(recall_next_boundary(50, 0) == -1);
    assert(recall_next_boundary(-5, BEAT) == 24);

    /* ---- the lead ---------------------------------------------------- */

    /* At 120bpm a pulse is 20.83ms, so 36ms of writes is one whole pulse. */
    assert(recall_lead_pulses(120.0f, BEAT, 36) == 1);
    /* At 60bpm a pulse is 41.7ms — 36ms does not even reach one. */
    assert(recall_lead_pulses(60.0f, BEAT, 36) == 0);
    /* At 240bpm a pulse is 10.4ms, so 36ms is three. */
    assert(recall_lead_pulses(240.0f, BEAT, 36) == 3);

    /*
     * CLAMPED BELOW THE DIVISION. Without this a fast tempo (or a long write
     * budget) makes the lead reach back past the PREVIOUS boundary, and
     * recall_should_fire is then true the instant it is armed — the feature
     * silently degrades to "off" exactly when the timing matters most.
     */
    assert(recall_lead_pulses(240.0f, BEAT, 100000) == BEAT - 1);
    assert(recall_lead_pulses(240.0f, BAR, 100000) == BAR - 1);
    assert(recall_lead_pulses(120.0f, 1, 36) == 0);

    /* A nonsense tempo falls back to 120 rather than producing a nonsense
     * lead: sampler_get_bpm can report a measured rate from two clock ticks
     * that arrived a second apart while the transport was starting. */
    assert(recall_lead_pulses(0.0f, BEAT, 36) == recall_lead_pulses(120.0f, BEAT, 36));
    assert(recall_lead_pulses(100000.0f, BEAT, 36) == recall_lead_pulses(120.0f, BEAT, 36));

    /* ---- firing ------------------------------------------------------ */

    assert(!recall_should_fire(10, 24, 1));
    assert(!recall_should_fire(22, 24, 1));
    assert(recall_should_fire(23, 24, 1));      /* one pulse early, as asked */
    assert(recall_should_fire(24, 24, 1));
    assert(recall_should_fire(30, 24, 1));      /* a missed frame still fires */
    assert(!recall_should_fire(1000, -1, 1));   /* nothing armed */

    /* With no lead it fires exactly on the boundary and not before. */
    assert(!recall_should_fire(23, 24, 0));
    assert(recall_should_fire(24, 24, 0));

    /* End to end: armed at pulse 5, beat division, 120bpm. */
    {
        int target = recall_next_boundary(5, BEAT);
        int lead = recall_lead_pulses(120.0f, BEAT, 36);
        assert(target == 24 && lead == 1);
        for (int p = 5; p < 23; p++) assert(!recall_should_fire(p, target, lead));
        assert(recall_should_fire(23, target, lead));
    }

    printf("PASS test_recall_quantize\n");
    return 0;
}
