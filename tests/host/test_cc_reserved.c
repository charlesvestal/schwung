/*
 * The reserved CC set, tested standalone.
 *
 * Header-only so this can run without the device toolchain, the same reason
 * fx_midi_filter.h is. The point of the test is not the arithmetic -- it is
 * that ONE rule answers for both maps. The chain DSP and the host each own a
 * CC map, and when this was two copies with a comment on each promising they
 * matched, that promise was the only thing holding them together.
 */
#include <stdio.h>
#include "cc_reserved.h"

static int fails = 0;
static void ok(int cond, const char *msg)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond) fails++;
}

int main(void)
{
    /* Bank select is a 14-bit PAIR, not a control: mapping a parameter there
     * is a trap, and other gear sends both unprompted. */
    ok(cc_reserved(0),  "CC 0 (bank select MSB) is refused");
    ok(cc_reserved(32), "CC 32 (bank select LSB) is refused");

    /* Move's own chain knobs, both spellings. Refused outright rather than
     * conditionally: see the header. */
    for (int cc = 71; cc <= 78; cc++) {
        char m[64]; snprintf(m, sizeof(m), "CC %d (chain knob, relative) is refused", cc);
        ok(cc_reserved(cc), m);
    }
    for (int cc = 102; cc <= 109; cc++) {
        char m[64]; snprintf(m, sizeof(m), "CC %d (chain knob, absolute) is refused", cc);
        ok(cc_reserved(cc), m);
    }

    /* The edges, which is where an off-by-one would hide. */
    ok(!cc_reserved(70),  "CC 70 is usable (just below the relative range)");
    ok(!cc_reserved(79),  "CC 79 is usable (just above it)");
    ok(!cc_reserved(101), "CC 101 is usable (just below the absolute range)");
    ok(!cc_reserved(110), "CC 110 is usable (just above it)");
    ok(!cc_reserved(1),   "CC 1 is usable -- the user decides, not the map");
    ok(!cc_reserved(127), "CC 127 is usable");

    /* Out of range is not an address. */
    ok(cc_reserved(-1),  "-1 is not an address");
    ok(cc_reserved(128), "128 is not an address");

    /* 112 usable numbers is the figure the design leans on: with nothing
     * auto-assigned, a user assigns what they use and never reaches it. If a
     * range is ever widened, this is what should fail first. */
    int usable = 0;
    for (int cc = 0; cc <= 127; cc++) if (!cc_reserved(cc)) usable++;
    ok(usable == 110, "110 of 128 numbers remain assignable");

    printf(fails ? "\n%d FAILED\n" : "\nall passed\n", fails);
    return fails ? 1 : 0;
}
