#ifndef RELATIVE_CC_H
#define RELATIVE_CC_H

/*
 * Relative (endless) encoder CC arithmetic, as two pure functions.
 *
 * WHY IT IS A HEADER. The call site is v2_on_midi in chain_midi.c, which
 * dlopens plugins and owns the get_param/set_param surface, so it cannot be
 * compiled natively — test_chain_knob_cc_out.c says exactly that in its own
 * preamble, and pins chain_midi.c at the SOURCE level in its companion .sh
 * instead. Source-level pinning cannot tell 4 from 8. So the arithmetic moves
 * here, the way recall_quantize.h, transport_grid.h, fx_midi_filter.h and
 * master_fx_key.h did before it, and tests/host/test_relative_cc.c runs it.
 *
 * THE ENCODING. Two's complement, 7-bit: 1..63 is +1..+63 detents, 127..65 is
 * -1..-63, and 0 and 64 are not movements. `127 == -1` is what makes this two's
 * complement rather than the signed-bit convention, and it is the reading the
 * rest of the tree already uses — schwung_shim.c (two sites), schwung_host.c,
 * and decodeDelta() in shared/input_filter.mjs. chain_midi.c was the last
 * decoder that read only +/-1 (#402).
 */

/*
 * Signed detent count for a relative CC value. 0 means "no movement", which
 * covers both the literal 0 and the midpoint 64 — a caller that treats the
 * return as a delta needs no separate guard, and one that wants to skip the
 * message entirely can test for zero.
 *
 * 64 is excluded rather than read as -64 because no endless encoder emits it:
 * the convention is 1..63 / 65..127 either side of an unused centre, which is
 * how every sibling decoder in the tree spells it (`d2 >= 65 && d2 <= 127`).
 */
static inline int relative_cc_ticks(int value) {
    if (value <= 0 || value > 127) return 0;
    if (value < 64) return value;
    if (value == 64) return 0;
    return value - 128;
}

/*
 * How many base steps one message is worth, given the detents it reported and
 * the time-based acceleration multiplier the caller computed.
 *
 * THE LARGER OF THE TWO, never the product, and never one replacing the other.
 * They measure the same thing by different means and only one of them is ever
 * real for a given controller:
 *
 *   - a plain encoder always says 1 detent, so `accel` is the only signal that
 *     the user is turning fast, and it must survive;
 *   - an accelerated encoder (OXI E16 and most endless controllers) batches
 *     detents into the magnitude, so `mag` already IS the acceleration.
 *
 * Multiplying them compounds two accelerations and makes a small turn cross the
 * whole range. But suppressing `accel` whenever `mag > 1` — the first shape
 * this took — puts a CLIFF on the fast side: with KNOB_ACCEL_MAX_MULT at 4, a
 * one-detent message turned fast was worth 4 base steps and a two-detent one
 * only 2, so the parameter moved LESS the harder it was driven. That is the
 * same symptom as the bug the decode fixed, one octave quieter.
 *
 * `max` is monotone in both inputs, has no crossover, and is the identity on
 * Move's own knobs, which only ever report one detent: max(1, accel) == accel,
 * bit-identical to the arithmetic that shipped before any of this.
 *
 * ENUMS NEED NO SPECIAL CASE, and that is load-bearing rather than lucky. The
 * caller pins `accel` to KNOB_ACCEL_ENUM_MULT (1) for an enum, which is what
 * "enums never accelerate" has always meant: no TIME-based multiplier, because
 * a deliberate slow turn must not overshoot a list of options. A detent count
 * is not acceleration — it is how far the user physically turned — so
 * max(mag, 1) == mag hands an enum exactly the options the user asked for.
 * Forcing mag to 1 instead made enums advance one option per MESSAGE, and an
 * accelerated encoder scans at a fixed rate and answers a faster spin with a
 * BIGGER message rather than more of them: options-per-second then stops
 * depending on how hard you turn, and a long enum cannot be crossed at all.
 * On Move that distinction never existed, because there messages and detents
 * are the same thing.
 */
static inline int relative_cc_multiplier(int mag, int accel) {
    if (mag < 1) mag = 1;
    if (accel < 1) accel = 1;
    return (mag > accel) ? mag : accel;
}

#endif /* RELATIVE_CC_H */
