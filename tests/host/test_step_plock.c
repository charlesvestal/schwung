/* test_step_plock.c - a held step button's phase in clip time.
 *
 * The forward mapping was measured on hardware (16 sightings on an 11/8 set,
 * then 26/26 on the phase check); this is its inverse, so the cases here are
 * the ones the device pinned plus every refusal.
 */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "step_plock.h"
#include "step_strip.h"

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

int main(void)
{
    double ph = 0.0;
    int rc;

    /* 1. 4/4 at 1/16: a bar is 16 steps, so the step buttons ARE the bar --
     * Move's own "the entire bar can be accessed at once". Step 1 of bar 1 is
     * phase 0; step 5 is one quarter in; bar 2 step 1 is 4 quarters in. */
    rc = step_plock_phase(1, 0, 4.0, 0.25, 16.0, &ph);
    CHECK(rc == STEP_PLOCK_OK && ph == 0.0, "bar1 step1: rc=%d ph=%f", rc, ph);
    rc = step_plock_phase(1, 4, 4.0, 0.25, 16.0, &ph);
    CHECK(rc == STEP_PLOCK_OK && ph == 1.0, "bar1 step5: rc=%d ph=%f", rc, ph);
    rc = step_plock_phase(1, 15, 4.0, 0.25, 16.0, &ph);
    CHECK(rc == STEP_PLOCK_OK && fabs(ph - 3.75) < 1e-12,
          "bar1 step16: rc=%d ph=%f, want 3.75", rc, ph);
    rc = step_plock_phase(3, 0, 4.0, 0.25, 16.0, &ph);
    CHECK(rc == STEP_PLOCK_OK && ph == 8.0,
          "bar3 step1: rc=%d ph=%f, want 8.0 -- and this is exactly the loop "
          "start the 11/8 measurement turned on", rc, ph);

    /* 2. A FINER GRID puts more steps in a bar than there are buttons, so the
     * bar is paged and WHICH page is displayed is a fact we do not have.
     * Refused, never guessed: a p-lock one page out is a value on the wrong
     * sixteenth, silently. */
    rc = step_plock_phase(1, 0, 4.0, 0.125, 16.0, &ph);   /* 1/32 in 4/4 = 32 */
    CHECK(rc == STEP_PLOCK_MULTI_PAGE && !isfinite(ph),
          "1/32 in 4/4 should refuse as multi-page: rc=%d ph=%f", rc, ph);
    /* And the case that made this real: 11/8 at 1/16 is 22 steps -- it pages
     * 16 + 6 even at the DEFAULT grid, which is why this refusal is not an
     * exotic edge. */
    rc = step_plock_phase(1, 0, 5.5, 0.25, 12.0, &ph);
    CHECK(rc == STEP_PLOCK_MULTI_PAGE,
          "11/8 at 1/16 should refuse as multi-page: rc=%d", rc);

    /* 3. A COARSER GRID makes a bar shorter than the buttons, and the buttons
     * past its end belong to no step of it. */
    rc = step_plock_phase(2, 0, 4.0, 0.5, 16.0, &ph);     /* 1/8 in 4/4 = 8 */
    CHECK(rc == STEP_PLOCK_OK && ph == 4.0, "1/8 bar2 step1: rc=%d ph=%f", rc, ph);
    rc = step_plock_phase(2, 7, 4.0, 0.5, 16.0, &ph);
    CHECK(rc == STEP_PLOCK_OK && fabs(ph - 7.5) < 1e-12,
          "1/8 bar2 step8: rc=%d ph=%f, want 7.5", rc, ph);
    rc = step_plock_phase(2, 8, 4.0, 0.5, 16.0, &ph);
    CHECK(rc == STEP_PLOCK_BAD_INDEX,
          "step 9 of an 8-step bar should be refused: rc=%d ph=%f", rc, ph);

    /* 4. A TRIPLET grid is just a smaller resolution, and 1/8t in 4/4 is 12
     * steps to the bar -- which fits, so it is answerable. */
    {
        const double res = 0.5 * 2.0 / 3.0;               /* an eighth triplet */
        rc = step_plock_phase(1, 3, 4.0, res, 16.0, &ph);
        CHECK(rc == STEP_PLOCK_OK && fabs(ph - 3.0 * res) < 1e-12,
              "1/8t bar1 step4 (index 3 = three steps in): rc=%d ph=%f, "
              "want %f", rc, ph, 3.0 * res);
        rc = step_plock_phase(1, 12, 4.0, res, 16.0, &ph);
        CHECK(rc == STEP_PLOCK_BAD_INDEX,
              "1/8t gives 12 steps per bar, so step 13 is not one: rc=%d", rc);
    }

    /* 5a. ...BUT THE STRIP NAMES BAR 1 WITHOUT THICKENING ANYTHING.
     *
     * Case 5 below is right about the pure function and was, on its own,
     * actively misleading: it pins the refusal and reads as though the
     * one-bar case is handled. It is not the function's to handle. A one-bar
     * loop draws a thin line and no bold segment, so the field the caller was
     * reading is 0 -- indistinguishable there from "no reading" -- and every
     * single-bar clip had its p-locks refused on hardware while this file was
     * green. That is the common short clip, and it is exactly what Move's
     * Shift+Step 14 (new clip) gives you.
     *
     * So the assertion that was missing is on the STEP BETWEEN the reader and
     * the function, which is why that step is now a named function rather
     * than an expression at the call site. */
    {
        step_strip_t ss;
        memset(&ss, 0, sizeof ss);

        ss.valid = 1; ss.segments = 1; ss.single_thin = 1; ss.bold_segment = 0;
        CHECK(step_strip_displayed_bar(&ss, 2, 2) == 1,
              "a one-bar loop's thin line IS bar 1, not 'unknown': got %d",
              step_strip_displayed_bar(&ss, 2, 2));

        /* ...and it still has to be THIS track's editor. */
        CHECK(step_strip_displayed_bar(&ss, 3, 2) == 0,
              "a reading paired with another track must not name a bar");

        ss.single_thin = 0; ss.segments = 4; ss.bold_segment = 3;
        CHECK(step_strip_displayed_bar(&ss, 2, 2) == 3,
              "a thickened segment is the displayed bar: got %d",
              step_strip_displayed_bar(&ss, 2, 2));

        /* An invalid reading names nothing, however inviting its fields look. */
        ss.valid = 0;
        CHECK(step_strip_displayed_bar(&ss, 2, 2) == 0,
              "an invalid strip must not name a bar");
        CHECK(step_strip_displayed_bar(NULL, 2, 2) == 0,
              "a NULL strip must not name a bar");

        /* And the whole point: what that resolution feeds must now SUCCEED
         * where the old expression refused. 4/4, 1/16, one bar = 16 steps. */
        double ph2 = 0.0;
        memset(&ss, 0, sizeof ss);
        ss.valid = 1; ss.segments = 1; ss.single_thin = 1;
        int rc2 = step_plock_phase(step_strip_displayed_bar(&ss, 2, 2), 3,
                                   4.0, 0.25, 4.0, &ph2);
        CHECK(rc2 == STEP_PLOCK_OK && fabs(ph2 - 0.75) < 1e-12,
              "a p-lock on a one-bar 4/4 clip must land at 0.75: rc=%d ph=%f",
              rc2, ph2);

        /* ...AND RESOLVING THE BAR IS NOT THE SAME AS UNBLOCKING THE SET.
         *
         * Measured on the device 2026-09-13: an 11/8 set on a 1/16 grid is
         * 5.5 / 0.25 = 22 steps to the bar against 16 buttons, so the bar
         * spans two pages and every p-lock on it refuses -- with the bar now
         * correctly named as 1. The two defects sat on top of each other and
         * the first hid the second, which is why this is pinned rather than
         * left as a comment: someone verifying the single_thin fix on that
         * set would see no change and reasonably conclude it had not worked.
         *
         * Lifting it needs a PAGE, which the strip does not name -- see
         * step_plock.h's formula, where `page` is already a term. */
        double ph3 = 0.0;
        int rc3 = step_plock_phase(step_strip_displayed_bar(&ss, 2, 2), 3,
                                   5.5, 0.25, 5.5, &ph3);
        CHECK(rc3 == STEP_PLOCK_MULTI_PAGE,
              "11/8 at 1/16 is 22 steps to the bar: expected MULTI_PAGE, rc=%d",
              rc3);
    }

    /* 4b. AN UNSAVED CLIP STILL HAS A LENGTH, AND 0 IS NOT IT.
     *
     * Move does not write a clip to Song.abl until it has notes, so a clip
     * made with Shift+Step 14 is selected, playable, and absent from the
     * file. The length then came out 0 -- and 0 does not mean "zero length"
     * here, it switches the OUTSIDE_CLIP check OFF, so a p-lock past the end
     * of a brand-new clip was accepted unbounded at exactly the moment a user
     * is filling one in. The strip names the bars and a new clip starts at 0,
     * so the length is segments * quarters_per_bar.
     *
     * Pinned as the two OUTCOMES rather than by calling the host's private
     * translation: with a length, the step past the end refuses. */
    {
        const double qpb = 4.0, res = 0.25;
        const double strip_len = 1 * qpb;   /* one segment = one bar */
        double ph4 = 0.0;

        /* Inside that one bar: fine either way. */
        CHECK(step_plock_phase(1, 15, qpb, res, strip_len, &ph4) == STEP_PLOCK_OK,
              "the last step of a one-bar clip must be accepted");

        /* Bar 2 step 1 is past a one-bar clip. WITH the strip length it is
         * refused; with the old 0 it was silently accepted -- which is the
         * whole defect, so assert both sides. */
        CHECK(step_plock_phase(2, 0, qpb, res, strip_len, &ph4)
                  == STEP_PLOCK_OUTSIDE_CLIP,
              "past the end of an unsaved clip must refuse once its length is "
              "known from the strip");
        CHECK(step_plock_phase(2, 0, qpb, res, 0.0, &ph4) == STEP_PLOCK_OK,
              "a length of 0 disables the bound -- if this ever refuses, the "
              "fallback above is no longer load-bearing and can go");
    }

    /* 5. NO BAR IS NOT BAR 1. The displayed bar comes off the strip, and a
     * one-bar loop draws no thickening at all, so `bold_segment` can be 0 --
     * which must refuse rather than silently mean the first bar. */
    rc = step_plock_phase(0, 0, 4.0, 0.25, 16.0, &ph);
    CHECK(rc == STEP_PLOCK_NO_BAR && !isfinite(ph),
          "bar 0 must refuse: rc=%d ph=%f", rc, ph);

    /* 6. An unusable grid or bar length refuses rather than dividing by it. */
    CHECK(step_plock_phase(1, 0, 4.0, 0.0, 16.0, &ph) == STEP_PLOCK_NO_GRID,
          "a zero step resolution was accepted");
    CHECK(step_plock_phase(1, 0, 4.0, NAN, 16.0, &ph) == STEP_PLOCK_NO_GRID,
          "a NaN step resolution was accepted");
    CHECK(step_plock_phase(1, 0, 0.0, 0.25, 16.0, &ph) == STEP_PLOCK_NO_GRID,
          "a zero bar length was accepted");
    CHECK(step_plock_phase(1, -1, 4.0, 0.25, 16.0, &ph) == STEP_PLOCK_BAD_INDEX,
          "a negative step index was accepted");
    CHECK(step_plock_phase(1, 16, 4.0, 0.25, 16.0, &ph) == STEP_PLOCK_BAD_INDEX,
          "there are 16 step buttons, so index 16 is not one");

    /* 7. PAST THE CLIP'S END is refused when the length is known -- Move lets
     * you page beyond a clip and a note there extends it, but a lane cannot
     * hold a phase the clip has no time for yet. */
    rc = step_plock_phase(5, 0, 4.0, 0.25, 16.0, &ph);
    CHECK(rc == STEP_PLOCK_OUTSIDE_CLIP,
          "bar 5 of a 4-bar clip should be refused: rc=%d ph=%f", rc, ph);
    /* ...and NOT refused when the length is unknown, which is the state of
     * every clip Move has not saved yet. Taking the gesture away there would
     * undo the rest of this feature's work. */
    rc = step_plock_phase(5, 0, 4.0, 0.25, -1.0, &ph);
    CHECK(rc == STEP_PLOCK_OK && ph == 16.0,
          "an unknown clip length must not refuse: rc=%d ph=%f", rc, ph);

    /* 8. THE INVERSE OF THE MEASURED FORWARD MODEL. For every step of a 4/4
     * bar at 1/16, the forward mapping the device scored 26/26 must return the
     * index we asked for -- that is what makes this the same fact rather than
     * a second one that happens to agree today. */
    for (int bar = 1; bar <= 4; bar++) {
        for (int idx = 0; idx < 16; idx++) {
            rc = step_plock_phase(bar, idx, 4.0, 0.25, 64.0, &ph);
            CHECK(rc == STEP_PLOCK_OK, "bar %d idx %d refused (%d)", bar, idx, rc);
            if (rc != STEP_PLOCK_OK) continue;
            int step_in_clip = (int)(ph / 0.25 + 0.5);
            int steps_per_bar = 16;
            int back = (step_in_clip % steps_per_bar) % 16;
            CHECK(back == idx,
                  "round trip: bar %d idx %d -> phase %f -> idx %d",
                  bar, idx, ph, back);
        }
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: step_plock\n");
    return 0;
}
