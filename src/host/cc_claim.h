#ifndef CC_CLAIM_H
#define CC_CLAIM_H
/*
 * Who owns an incoming external (cable 2) Control Change -- the generic CC
 * map's half of the ownership order in
 * docs/superpowers/specs/2026-09-26-custom-surface-layout-design.md:
 *
 *   1. an active remote surface's protocol (e16_claim.h)    -- checked FIRST
 *   2. a CC-map binding (this table)                        -- here
 *   3. nobody: the message goes on to Move and the slots
 *
 * The shadow UI writes the table (`schwung_cc_claim_t`, /schwung-cc-claim):
 * one bit per (channel, CC) that a binding owns, a count so an empty table
 * costs one comparison, and a LEARN flag. The shim reads it in its cable-2
 * walk and asks this header what to do with each CC:
 *
 *   publish   hand the message to the shadow UI (the CC map acts on it)
 *   swallow   take it out of BOTH MIDI_IN buffers, so Move and the slot
 *             synths do not ALSO act on a CC that is now bound to something
 *
 * A bound CC is published and swallowed. While the UI is LEARNING a CC, every
 * CC is published so it can be chosen -- and NOT swallowed, because nothing
 * owns it yet: a controller must keep working normally while you pick.
 *
 * HEADER-ONLY AND PURE, like e16_claim.h: the call site is the SPI callback,
 * which cannot be compiled on a dev machine; this can, so tests/host runs it.
 */
#include <stdint.h>

#define CC_CLAIM_VERSION  1
#define CC_CLAIM_CHANNELS 16
#define CC_CLAIM_CCS      128
#define CC_CLAIM_BYTES    (CC_CLAIM_CHANNELS * CC_CLAIM_CCS / 8)   /* 256 */

/* Is (channel 0-15, cc 0-127) bound? */
static inline int cc_claim_owns(const volatile uint8_t *bits, int channel, int cc) {
    if (!bits || channel < 0 || channel >= CC_CLAIM_CHANNELS || cc < 0 || cc >= CC_CLAIM_CCS) return 0;
    const int bit = channel * CC_CLAIM_CCS + cc;
    return (bits[bit >> 3] >> (bit & 7)) & 1;
}

#define CC_ROUTE_PUBLISH 1
#define CC_ROUTE_SWALLOW 2

/*
 * What to do with one cable-2 message, given the table. Only a Control Change
 * is ever claimed here -- notes, pitch bend and SysEx pass through untouched
 * (a CC map is not a note map, and SysEx has its own consumers). `count` 0 and
 * `learn` 0 is the whole cost for a user with no bindings.
 */
static inline int cc_claim_route(uint8_t count, uint8_t learn, const volatile uint8_t *bits,
                                 int status, int cc) {
    if ((status & 0xF0) != 0xB0) return 0;
    if (!count && !learn) return 0;
    if (count && cc_claim_owns(bits, status & 0x0F, cc)) return CC_ROUTE_PUBLISH | CC_ROUTE_SWALLOW;
    return learn ? CC_ROUTE_PUBLISH : 0;
}

/* Set or clear one bit (the shadow UI's writer; also used by the test). */
static inline void cc_claim_set(volatile uint8_t *bits, int channel, int cc, int on) {
    if (!bits || channel < 0 || channel >= CC_CLAIM_CHANNELS || cc < 0 || cc >= CC_CLAIM_CCS) return;
    const int bit = channel * CC_CLAIM_CCS + cc;
    if (on) bits[bit >> 3] |= (uint8_t)(1u << (bit & 7));
    else bits[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
}

#endif /* CC_CLAIM_H */
