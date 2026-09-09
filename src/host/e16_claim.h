#ifndef E16_CLAIM_H
#define E16_CLAIM_H

/*
 * Who owns CC 1-16 on channel 1: the OXI E16 remote surface, or the chain's
 * CC Map (the "Handle knob CC mappings" lookup in chain_midi.c).
 *
 * WHY THIS EXISTS. In its remote mode the E16 emits every encoder turn as
 * CC 1-16 on channel 1 — a fixed assignment forced by the device, not a
 * setting either side can change. A user who already mapped CC 1 to a
 * parameter through the CC Map would have BOTH the surface's own consumer and
 * the CC Map act on the same message: the parameter moves twice as far as one
 * turn should move it, with nothing logged, because both features are
 * behaving exactly as designed. Neither side can see the other's claim
 * without this — the CC Map has no idea a surface exists, and the surface has
 * no idea a CC got mapped.
 *
 * The claim is a CC NUMBER + CHANNEL fact, not a feature of either consumer,
 * so it is defined once, here, and both sides read it: the CC Map's lookup in
 * chain_midi.c refuses a claimed message while the surface is active, and a
 * future E16 input consumer reads the same range to know what it owns.
 *
 * HEADER-ONLY, for the same reason as cc_reserved.h, recall_quantize.h,
 * transport_grid.h, fx_midi_filter.h and master_fx_key.h before it: the call
 * site is chain_midi.c's v2_on_midi, which is the SPI callback and dlopens
 * plugins, so it cannot be compiled natively. The predicate moves here so
 * tests/host can compile and run it on a dev machine.
 *
 * PURE ON PURPOSE. `shadow_control_t.external_surface` is the real flag, but
 * this header must not depend on that struct — it would pull in the whole
 * shadow SHM layout for one bit, and chain_midi.c's translation unit has no
 * reason to know that struct's shape. The caller reads the flag from
 * whatever it is wired to and passes the resulting 0/1 in.
 */

/* The E16's fixed remote-mode encoder assignment: CC 1-16, channel 1 (wire
 * value 0 — MIDI channels are 0-based on the status byte, 1-based in every
 * UI). This is the device's own protocol, not a Schwung choice, so it is a
 * constant rather than a setting. */
#define E16_CLAIMED_CC_LOW   1
#define E16_CLAIMED_CC_HIGH  16
#define E16_CLAIMED_CHANNEL  0

/*
 * Does the E16 surface own this (channel, cc) pair right now?
 *
 * `surface_active` is the caller's read of the external-surface flag — 0 when
 * the E16 is not the active surface, non-zero when it is. While active, CC
 * 1-16 on channel 1 belong to the surface alone: the CC Map's lookup must
 * return without touching its mapping table for a claimed message, exactly as
 * if the message had never arrived.
 *
 * While `surface_active` is 0 this always returns 0, so nothing changes for
 * anyone not using the surface — the CC Map behaves exactly as it did before
 * this header existed.
 */
static inline int e16_claims_cc(int surface_active, int channel, int cc) {
    if (!surface_active) return 0;
    if (channel != E16_CLAIMED_CHANNEL) return 0;
    return cc >= E16_CLAIMED_CC_LOW && cc <= E16_CLAIMED_CC_HIGH;
}

#endif /* E16_CLAIM_H */
