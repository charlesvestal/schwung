/*
 * Relative (endless) encoder CC arithmetic — relative_cc.h.
 *
 * The call site is v2_on_midi in chain_midi.c, which cannot be compiled
 * natively (it dlopens plugins and owns the get_param/set_param surface), so
 * its companion .sh pins it at the SOURCE level. That is enough to catch a
 * deleted call and useless for everything below: source-level pinning cannot
 * tell 4 base steps from 8, which is precisely where both bugs lived.
 *
 * Four properties, and each of them shipped wrong at some point:
 *
 *   1. THE FULL RANGE DECODES. Before #402 only 1 and 127 did, so every fast
 *      turn of an accelerated encoder was dropped. The control worked when
 *      crept and died when played.
 *   2. MOVE IS BIT-IDENTICAL. Move's own knobs only ever report one detent,
 *      so max(1, accel) == accel must hold for every accel — this is the whole
 *      reason the change is safe to ship.
 *   3. NO CLIFF ON THE FAST SIDE. #402's first shape suppressed accel whenever
 *      mag > 1, so a one-detent fast message was worth 4 and a two-detent one
 *      only 2: the parameter moved LESS the harder it was driven.
 *   4. ENUMS TAKE THE DETENT COUNT. Pinning mag to 1 made them advance one
 *      option per MESSAGE, and an accelerated encoder answers a faster spin
 *      with a bigger message rather than more of them — so a long enum could
 *      not be crossed at any speed.
 *
 * Properties 3 and 4 are monotonicity claims, so they are asserted as
 * monotonicity over the whole input space rather than at a couple of points: a
 * cliff is exactly what a spot check steps over.
 */
#include <stdio.h>

#include "relative_cc.h"

/* Mirrors of the chain's constants. Restated rather than included because
 * chain_internal.h pulls in the whole plugin surface; the .sh companion fails
 * if these drift from the header that ships. */
#define KNOB_ACCEL_MIN_MULT      1
#define KNOB_ACCEL_MAX_MULT      4
#define KNOB_ACCEL_MAX_MULT_INT  2
#define KNOB_ACCEL_ENUM_MULT     1

static int failures = 0;

static void check(int cond, const char *what) {
    if (!cond) { printf("  FAIL: %s\n", what); failures++; }
}

static void check_eq(int got, int want, const char *what) {
    if (got != want) {
        printf("  FAIL: %s — got %d, want %d\n", what, got, want);
        failures++;
    }
}

/* 1. The full two's-complement range, and the two non-movements. */
static void test_decode_range(void) {
    printf("== decode: the full range, not just +/-1 ==\n");

    check_eq(relative_cc_ticks(1), 1, "value 1 is +1 detent");
    check_eq(relative_cc_ticks(63), 63, "value 63 is +63 detents");
    check_eq(relative_cc_ticks(127), -1, "value 127 is -1 detent (two's complement, not signed-bit)");
    check_eq(relative_cc_ticks(65), -63, "value 65 is -63 detents");

    check_eq(relative_cc_ticks(0), 0, "value 0 is not a movement");
    check_eq(relative_cc_ticks(64), 0, "value 64 is the unused midpoint");

    /* Out of range for a MIDI data byte; answer 0 rather than sign-extending
     * whatever a caller happened to pass. */
    check_eq(relative_cc_ticks(128), 0, "128 is not a data byte");
    check_eq(relative_cc_ticks(-1), 0, "negative is not a data byte");

    /* THE BUG: everything except 1 and 127 used to return nothing. Asserted
     * over the whole range so a decoder that handles only a few extra values
     * cannot pass. */
    int moved = 0;
    for (int v = 1; v <= 127; v++) if (relative_cc_ticks(v) != 0) moved++;
    check_eq(moved, 126, "every value but 0 and 64 is a movement");

    /* Symmetric: +n and its negative counterpart are the same magnitude. */
    for (int n = 1; n <= 63; n++) {
        check_eq(relative_cc_ticks(n), -relative_cc_ticks(128 - n),
                 "positive and negative counterparts are symmetric");
    }
}

/* 2. Move's own knobs. One detent, every accel value, unchanged. */
static void test_move_is_bit_identical(void) {
    printf("== Move's knobs: max(1, accel) == accel ==\n");
    for (int accel = KNOB_ACCEL_MIN_MULT; accel <= KNOB_ACCEL_MAX_MULT; accel++) {
        check_eq(relative_cc_multiplier(1, accel), accel,
                 "a one-detent message is worth exactly the time-based multiplier");
    }
}

/* 3. No cliff: the multiplier never DROPS as the encoder reports more detents. */
static void test_no_cliff_on_the_fast_side(void) {
    printf("== no cliff: more detents never means less movement ==\n");

    /* The exact case that regressed: one detent turned fast beat two detents. */
    check(relative_cc_multiplier(2, KNOB_ACCEL_MAX_MULT) >=
          relative_cc_multiplier(1, KNOB_ACCEL_MAX_MULT),
          "two detents is not worth less than one at max acceleration");

    /* And the general claim, over every magnitude and every accel the caller
     * can produce (floats reach MAX_MULT, ints are capped at MAX_MULT_INT,
     * enums are pinned to ENUM_MULT — all covered by the accel loop). */
    for (int accel = KNOB_ACCEL_MIN_MULT; accel <= KNOB_ACCEL_MAX_MULT; accel++) {
        int prev = relative_cc_multiplier(1, accel);
        for (int mag = 2; mag <= 63; mag++) {
            int cur = relative_cc_multiplier(mag, accel);
            check(cur >= prev, "multiplier is monotone in the detent count");
            prev = cur;
        }
    }

    /* Monotone in the other input too, so a faster turn of a plain encoder
     * never moves the parameter less. */
    for (int mag = 1; mag <= 63; mag++) {
        int prev = relative_cc_multiplier(mag, KNOB_ACCEL_MIN_MULT);
        for (int accel = KNOB_ACCEL_MIN_MULT + 1; accel <= KNOB_ACCEL_MAX_MULT; accel++) {
            int cur = relative_cc_multiplier(mag, accel);
            check(cur >= prev, "multiplier is monotone in the acceleration");
            prev = cur;
        }
    }

    /* Never the product: that is what makes a small turn cross the range. */
    check(relative_cc_multiplier(8, KNOB_ACCEL_MAX_MULT) < 8 * KNOB_ACCEL_MAX_MULT,
          "the two multipliers do not compound");
}

/* 4. An enum crosses at the speed the user turns, not at the message rate. */
static void test_enum_takes_the_detent_count(void) {
    printf("== enums: options follow detents, not messages ==\n");

    /* The caller pins accel to ENUM_MULT, which is what "enums never
     * accelerate" means — no TIME-based multiplier. The detent count is not
     * acceleration, so it survives. */
    check_eq(relative_cc_multiplier(1, KNOB_ACCEL_ENUM_MULT), 1,
             "one detent still advances exactly one option (Move, unchanged)");
    check_eq(relative_cc_multiplier(5, KNOB_ACCEL_ENUM_MULT), 5,
             "five detents advance five options");
    check_eq(relative_cc_multiplier(63, KNOB_ACCEL_ENUM_MULT), 63,
             "a full-magnitude message is not clamped to one option");

    /* THE BUG, as the property it broke: with mag pinned to 1, a spin that
     * batches detents delivers the same number of options however hard it is
     * turned. Assert that options-per-message actually tracks the spin. */
    check(relative_cc_multiplier(8, KNOB_ACCEL_ENUM_MULT) >
          relative_cc_multiplier(2, KNOB_ACCEL_ENUM_MULT),
          "a harder spin crosses more of a long enum");
}

int main(void) {
    test_decode_range();
    test_move_is_bit_identical();
    test_no_cliff_on_the_fast_side();
    test_enum_takes_the_detent_count();

    if (failures) {
        printf("FAILED (%d)\n", failures);
        return 1;
    }
    printf("PASS: relative CC decode + multiplier\n");
    return 0;
}
