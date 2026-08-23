/*
 * KNOB LEDS. Ported from schwung-movy src/renderer/knob-leds.ts, with
 * permission.
 *
 * The movy grid draws 8 parameters as two rows of four; the hardware is one row
 * of eight encoders. Nothing on the device says which physical knob drives
 * which drawn cell — so the LEDs do: knobs 1-4 white, knobs 5-8 amber.
 *
 * VALUE RIDES ON TOP AS INTENSITY, AND THE FLOOR IS NOT ZERO. Every bound knob
 * stays lit however low its value, because the row identity has to survive a
 * parameter sitting at 0. Colour 0 is reserved for "nothing is bound here",
 * which is the whole of "only controls that do something are lit" — a dark knob
 * is a knob that will do nothing if you turn it.
 *
 * CC 71-78, AND NOTHING ELSE. The same CC carries encoder rotation IN and the
 * indicator ring colour OUT: schwung-spi's schwung_move_ui.h:193 ("Knob
 * indicator ring LEDs (RGB)", "Same CC as encoder rotation") and its :386
 * classification of them as CC-addressed LEDs, plus the extending-move wiki's
 * LED table. Notes 0-7 are TOUCH SENSORS, input only — constants.mjs annotates
 * the step notes "and LED" and these deliberately not. movy writes both, with a
 * comment saying the LED type is unconfirmed; it is confirmed, so the notes
 * half is dropped. It was eight wasted packets per change into a buffer that
 * holds about 64.
 *
 * WHY THIS KEEPS ITS OWN DIFF CACHE. setLED/setButtonLED keep a module-level
 * cache we cannot invalidate, and the overtake LED-clear writes straight
 * through move_midi_internal_send without updating it — so any path where that
 * cache outlives a hardware clear leaves it claiming a colour the knob no
 * longer shows. We pass force=true to bypass it and diff here instead. That
 * makes THIS cache the only thing standing between a knob grid and 8 MIDI sends
 * every tick.
 */
import { setButtonLED } from "../input_filter.mjs";
import { MoveKnob1, DarkGrey2, LightGrey, White,
         DarkBrown2, Mustard, Ochre, BrightOrange } from "../constants.mjs";

export const NUM_KNOB_LEDS = 8;

/*
 * The two ramps, by NAME rather than by the raw index movy used — its hex
 * comments did not match our palette (it claimed #1A1A1A for 124, which is
 * #141414 here), so the indices were picked by eye on a device. The names are
 * what constants.mjs actually defines; if a ramp reads wrong on hardware,
 * re-pick from there rather than nudging a number.
 */
export const WHITE_LEVELS = [DarkGrey2, LightGrey, White];
export const AMBER_LEVELS = [DarkBrown2, Mustard, Ochre, BrightOrange];

const lastKnobColor = new Array(NUM_KNOB_LEDS).fill(-1);

/** Drop the cache so the next update re-emits every knob. */
export function resetKnobLedCache() { lastKnobColor.fill(-1); }

/**
 * The colour for one knob.
 *
 * @param {number} k    physical knob index, 0-7
 * @param {number|null} nv normalised 0..1, or null/undefined when unbound or
 *                      unread — see normalizedOf. Both are colour 0: an unlit
 *                      knob already reads as "nothing to turn here", which is
 *                      true of a key we could not read too, and lighting it at
 *                      the bottom of its range would be a confident lie.
 */
export function knobLedColor(k, nv) {
    if (nv === null || nv === undefined || !isFinite(nv)) return 0;
    const v = Math.max(0, Math.min(1, nv));
    if (k < 4) {
        if (v < 0.33) return WHITE_LEVELS[0];
        if (v < 0.67) return WHITE_LEVELS[1];
        return WHITE_LEVELS[2];
    }
    if (v < 0.25) return AMBER_LEVELS[0];
    if (v < 0.5)  return AMBER_LEVELS[1];
    if (v < 0.75) return AMBER_LEVELS[2];
    return AMBER_LEVELS[3];
}

/**
 * Push the 8 knob LEDs. Emits only what changed.
 *
 * @param {Array<number|null>} values normalised 0..1 per knob, null if unbound
 * @param {object} [io] injected by tests; defaults to the real LED helper
 */
export function updateKnobLEDs(values, io) {
    const btn = (io && io.setButtonLED) || setButtonLED;
    for (let k = 0; k < NUM_KNOB_LEDS; k++) {
        const color = knobLedColor(k, values ? values[k] : null);
        if (lastKnobColor[k] === color) continue;
        lastKnobColor[k] = color;
        btn(MoveKnob1 + k, color, true);
    }
}

/** Darken every knob — for leaving the grid. */
export function clearKnobLEDs(io) {
    updateKnobLEDs(new Array(NUM_KNOB_LEDS).fill(null), io);
}
