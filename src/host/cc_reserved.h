#ifndef CC_RESERVED_H
#define CC_RESERVED_H

/*
 * CC numbers a parameter may not be assigned.
 *
 * Header-only, and shared, for two reasons. The chain DSP and the host each own
 * a CC map -- a slot's and Master FX's -- and they must refuse the same numbers
 * or the same controller behaves differently on the two buses. And a rule
 * restated in two places is a rule that drifts: this started as two copies with
 * a comment on each promising they matched.
 *
 * Header-only also lets tests/host run it without the device toolchain, the
 * same reason fx_midi_filter.h is one.
 *
 *   0, 32      bank select MSB/LSB -- a 14-bit pair, not a control
 *   71-78      Move's own chain knobs, relative
 *   102-109    the same eight knobs, absolute
 *
 * The knob ranges are refused OUTRIGHT, not merely while a slot has knob
 * mappings. Both legacy blocks run before the map in chain_midi.c, so a
 * parameter assigned there works until somebody assigns a chain knob and then
 * silently stops. A number that works until something unrelated changes is
 * worse than one that cannot be picked at all.
 *
 * 18 of 128. With nothing auto-assigned, a user assigns what they use, so the
 * remaining 110 per slot is not a limit anybody reaches.
 */
static inline int cc_reserved(int cc)
{
    if (cc < 0 || cc > 127) return 1;      /* not an address either */
    if (cc == 0 || cc == 32) return 1;
    if (cc >= 71 && cc <= 78) return 1;
    if (cc >= 102 && cc <= 109) return 1;
    return 0;
}

#endif /* CC_RESERVED_H */
