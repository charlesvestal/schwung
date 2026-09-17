/* The lane lookahead — see src/host/lane_lookahead.h.
 *
 * The defect this pins: a p-lock whose rectangle starts exactly on a step was
 * evaluated a block LATE, so the note for that step latched the old value and
 * the lock appeared on the next hit.
 */
#include <stdio.h>
#include <math.h>

#include "lane_lookahead.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } } while (0)

int main(void) {
    const double lo = 0.0, len = 4.0;
    const double block = 0.0064;          /* ~128 frames at 133 BPM, in quarters */

    /* THE BUG. Standing one block before a step, the value read must be the
     * one that belongs to the STEP -- the note for it is delivered later in
     * this same frame. */
    {
        const double step = 1.5;
        const double now = step - block;
        const double e = lane_lookahead_phase(now, now - block, 1, lo, len);
        CHECK(e >= step, "a tick one block before the step evaluated at %.6f, "
                         "short of the step at %.6f -- the note would latch "
                         "the pre-lock value", e, step);
    }

    /* No previous phase: no lead. Late for one tick, never wrong. */
    {
        const double e = lane_lookahead_phase(1.5, 0.0, 0, lo, len);
        CHECK(e == 1.5, "a first tick invented a lead (%.6f)", e);
    }

    /* A WRAP is a negative delta and must not be used as a lead. */
    {
        const double e = lane_lookahead_phase(0.01, 3.99, 1, lo, len);
        CHECK(e == 0.01, "a wrap produced a lead (%.6f)", e);
    }

    /* A RE-ANCHOR is a large delta. Refused, not clamped: a lead that big
     * would play the NEXT step's lock on this step. */
    {
        const double e = lane_lookahead_phase(2.0, 1.0, 1, lo, len);
        CHECK(e == 2.0, "a 1-beat jump was used as a lead (%.6f)", e);
    }

    /* Past the loop end it must WRAP, or every lane releases for one block at
     * each loop boundary. */
    {
        const double now = lo + len - block / 2.0;
        const double e = lane_lookahead_phase(now, now - block, 1, lo, len);
        CHECK(e >= lo && e < lo + len,
              "a lookahead past the loop end landed outside it (%.6f, window "
              "[%.3f, %.3f))", e, lo, lo + len);
    }

    /* A window that does not start at 0 -- a bars-3-to-5 loop. */
    {
        const double lo2 = 8.0, len2 = 4.0;
        const double now = lo2 + len2 - block / 2.0;
        const double e = lane_lookahead_phase(now, now - block, 1, lo2, len2);
        CHECK(e >= lo2 && e < lo2 + len2,
              "wrapped outside a shifted window (%.6f)", e);
    }

    /* Degenerate inputs are passed through rather than turned into a value. */
    {
        const double e = lane_lookahead_phase(1.0, 0.99, 1, lo, 0.0);
        CHECK(e == 1.0, "a zero-length loop produced a lead (%.6f)", e);
    }

    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    printf("PASS: lane_lookahead\n");
    return 0;
}
