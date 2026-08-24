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
import { MoveKnob1, DarkGrey2, DarkGrey3, LightGrey, OffWhite, White,
         DarkBrown, BurntSienna, Tan, BrightOrange } from "../constants.mjs";

export const NUM_KNOB_LEDS = 8;

/*
 * THE TWO RAMPS ARE ORDERED BY LUMINANCE, WHICH IS NOT THE SAME AS BY NAME.
 *
 * The first version picked its constants by what they were called, and the
 * amber ramp was DarkBrown2 -> Mustard -> Ochre -> BrightOrange. Those are
 * #250E05 -> #876700 -> #491804 -> #C93C00: the third step is DARKER than the
 * second, so a knob swept from minimum to maximum went dim, bright, dark,
 * bright. Reported from the device as "the LEDs work but the curve is off /
 * weird", which is exactly what that is.
 *
 * The palette header in constants.mjs is the authority, and it already answers
 * this: every hue lists a `dim` and a `dark` variant, e.g.
 *
 *     3 : #C93C00  Bright Orange   dim  69 #5D1700   dark  70 #200D00
 *
 * So a ramp is one hue's dark -> dim -> full, optionally with a neighbour of
 * the same hue filling a gap — NOT a walk through whatever entries sound
 * warm. Mustard (#876700) and Ochre (#491804) are different hues at different
 * brightnesses and never belonged in one ramp.
 *
 * Verify a change by reading the hex out of that header, not by the name:
 *
 *     white  #141414  #1A1A1A(*)  #404040  #595959  #CCCCCC  #FFFFFF
 *     amber  #200D00  #5D1700     #AC1F00  #C93C00
 *
 * (*) DarkGrey #1A1A1A is skipped: it is within 2% of DarkGrey2 #141414, so
 * it costs a step of the ramp and shows nothing for it.
 *
 * The rows still differ by HUE at every level, which is the property that has
 * to survive — a white knob at minimum must not be mistakable for an amber one
 * at minimum.
 */
export const WHITE_LEVELS = [DarkGrey2, DarkGrey3, LightGrey, OffWhite, White];
export const AMBER_LEVELS = [DarkBrown, BurntSienna, Tan, BrightOrange];

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
    const ramp = k < 4 ? WHITE_LEVELS : AMBER_LEVELS;
    /*
     * The step boundaries are DERIVED from the ramp, not written beside it.
     * They were written beside it — `v < 0.33`, `v < 0.67` against a 3-entry
     * white ramp and quarters against a 4-entry amber one — so lengthening
     * either ramp silently left its last entries unreachable. That is the
     * "green matrix only proves the axis you chose" failure in miniature: the
     * ramp and the thresholds are one fact and belong in one place.
     */
    const i = Math.min(ramp.length - 1, Math.floor(v * ramp.length));
    return ramp[i];
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
