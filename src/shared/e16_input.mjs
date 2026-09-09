/*
 * OXI E16 remote-mode input decode.
 *
 * WHY THIS IS FIXED, NOT CONFIGURABLE. In remote mode the E16 does not care
 * how its owner has mapped its 16 encoders on the device itself -- it always
 * emits CC 1-16 on MIDI channel 1 for turns (relative, accelerated), notes
 * 0-15 on channel 1 for the encoder pushes, and note 16 for Shift. A surface
 * driver that read this from a user-editable map could be broken by the same
 * knob the user turns to configure something else; keeping it fixed here
 * means decode() is correct regardless of what the E16's own menus say.
 *
 * THE RELATIVE ENCODING must agree with src/modules/chain/dsp/relative_cc.h
 * bit for bit: two's complement, 7-bit. 1..63 is +1..+63 detents, 127..65 is
 * -1..-63, and 0 and 64 both mean "no movement" (excluded because no endless
 * encoder emits them as a delta). That header exists because chain_midi.c
 * decoded only +/-1 for months and silently dropped every fast turn (#402) --
 * the only symptom was that knobs felt slow. A second, drifted copy of this
 * arithmetic here would reintroduce exactly that bug for the E16 specifically,
 * so keep the two definitions in lockstep by inspection whenever either one
 * changes; decodeDelta() in src/shared/input_filter.mjs is the same reading
 * again, a third time, for Move's own hardware encoders.
 */

/* MIDI channel the E16 uses for everything in remote mode (0-indexed: ch 1). */
const E16_CHANNEL = 0;

/* Note 16 is Shift -- outside the 0-15 range that maps to an encoder push. */
export const SHIFT_NOTE = 16;

/*
 * Signed detent count for a relative CC value, or 0 for "no movement".
 * Mirrors relative_cc_ticks() in relative_cc.h exactly.
 */
function relativeTicks(value) {
    if (value <= 0 || value > 127) return 0;
    if (value < 64) return value;
    if (value === 64) return 0;
    return value - 128;
}

/*
 * Decode one raw MIDI message from an E16 in remote mode into a surface
 * event, or null if the message is not one of the fixed events remote mode
 * defines. Returning null rather than a partially-filled event means a
 * caller can never act on an event whose fields do not all agree with each
 * other -- there is no "turn with no enc" or "push on the wrong channel".
 *
 * msg is [status, d1, d2] as delivered over MIDI_IN.
 */
export function decode(msg) {
    const status = msg[0];
    const d1 = msg[1];
    const d2 = msg[2];
    const type = status & 0xf0;
    const channel = status & 0x0f;

    if (channel !== E16_CHANNEL) return null;

    if (type === 0xb0) {
        /* CC 1-16 -> encoders 0-15 turning. */
        if (d1 < 1 || d1 > 16) return null;
        const ticks = relativeTicks(d2);
        if (ticks === 0) return null;
        return { type: "turn", enc: d1 - 1, ticks };
    }

    if (type === 0x90 || type === 0x80) {
        /* Note-on with velocity 0 is a release, same as everywhere else in
         * MIDI -- there is no separate "off" status byte to rely on. */
        const isOn = type === 0x90 && d2 > 0;

        if (d1 === SHIFT_NOTE) {
            return { type: "shift", down: isOn };
        }
        if (d1 >= 0 && d1 <= 15) {
            return isOn
                ? { type: "push", enc: d1 }
                : { type: "release", enc: d1 };
        }
        return null;
    }

    return null;
}
