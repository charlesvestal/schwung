/**
 * knob_engine.mjs — THE knob model. One implementation, one file.
 *
 * Every physical knob turn in the UI comes through here: the Movy knob grid,
 * the hierarchy list editor and its overlay, the patch editor, the waveform
 * zoom and marker knobs.
 *
 * There used to be two. This file held a time-based divisor curve (16 / 8 / 4
 * by how fast you turned) stepping by the module's DECLARED step, while
 * param_pages/movy_knob.mjs held the ported Movy model, which normalises a
 * step to a fraction of the parameter's own RANGE. A knob therefore behaved
 * differently depending on which screen you touched it from, reported from
 * the device as knobs being "super slow in the overlay". The gap was not
 * subtle:
 *
 *     float 20..20000 step 1      overlay 79,917 detents      grid 200
 *     int   0..127                overlay    505 detents      grid 127
 *
 * ...and shift-fine, which is meant to work everywhere, was honoured for
 * floats in one and ignored for ints and enums, while in the other it could
 * not move an int at all.
 *
 * The Movy model won: it is the one the knob grid ships with, so it is the
 * feel already in people's hands, and normalising to the range is what makes
 * every knob cross in about the same wrist movement whatever its units. What
 * was lost is the time-based acceleration — the range normalisation replaces
 * what it existed to paper over.
 *
 * An intermediate version kept both files, with this one delegating to the
 * other. That is still two places to look and two things to keep in step, so
 * the model was moved here and movy_knob.mjs deleted. There is also only one
 * ENTRY POINT: knobStep, taking metadata. The config-shaped knobTick /
 * knobConfigFromMeta pair the older call sites used is gone rather than
 * aliased -- an alias is a second name for the next reader to wire something
 * up to, which is how the two models drifted apart to begin with.
 *
 * ORIGIN: the model below is ported from schwung-movy
 * (`src/model/{knob-step,store,constants}.ts`, (c) 2026 megadake, MIT —
 * https://github.com/DimaDake/schwung-movy).
 *
 * The core idea: a float/int knob's per-detent step is a fixed FRACTION of
 * its own range (~1%, MIN_STEP_RANGE_FRAC) rather than the module's declared
 * `step`, which is a statement about precision and not about sweep. That is
 * what fixes both a wide-range knob crawling and a narrow one being a hair
 * trigger.
 */

import { isBooleanMeta } from "./param_pages/viz.mjs";

/*
 * The type tags the older call sites build their configs with. They name the
 * same three kinds the metadata does.
 */
export const KNOB_TYPE_FLOAT = "float";
export const KNOB_TYPE_INT = "int";
export const KNOB_TYPE_ENUM = "enum";

/** Physical clicks per value step for a narrow int range, and for every enum. */
export const ENUM_DELTA_DIV = 4;
/** Sensitivity multiplier for a continuous arc-rendered knob — every knob in
 * the Movy layout is one, so this always applies to float/int. */
export const ARC_DELTA_SCALE = 0.5;
/** A float/int knob's per-detent step, as a fraction of its own range. */
export const MIN_STEP_RANGE_FRAC = 0.01;
/** An int range this narrow or narrower is stepped like an enum. */
export const NARROW_RANGE_MAX = 8;

/** schwung-movy model/knob-step.ts detentsPerStep, ported. */
export function detentsPerStep(meta) {
    if (meta.type !== "int" || meta.knobAcceleration === "wide") return 1;
    const range = meta.max - meta.min;
    return (range >= 2 && range <= NARROW_RANGE_MAX) ? ENUM_DELTA_DIV : 1;
}

/** schwung-movy model/knob-step.ts perDetentStep, ported. */
export function perDetentStep(meta) {
    const arcScale = ARC_DELTA_SCALE;
    if (!(meta.max > meta.min)) return (meta.step > 0 ? meta.step : 0.01) * arcScale;
    const rangeStep = (meta.max - meta.min) * MIN_STEP_RANGE_FRAC;
    if (meta.type === "int") return Math.round(Math.max(meta.step > 0 ? meta.step : 1, rangeStep) * arcScale);
    return rangeStep * arcScale;   // float
}

/**
 * schwung-movy model/store.ts wideStepCount, ported. Velocity multiplier for
 * a param that opts into `knobAcceleration: "wide"` — see the module doc:
 * nothing in the fleet declares this today.
 */
function wideStepCount(state, direction, nowMs) {
    let multiplier = 1;
    const elapsed = state.lastTurnMs > 0 ? nowMs - state.lastTurnMs : Infinity;
    if (direction === state.lastDirection) {
        if (elapsed <= 35) multiplier = 250;
        else if (elapsed <= 90) multiplier = 50;
        else if (elapsed <= 180) multiplier = 10;
    }
    state.lastTurnMs = nowMs;
    state.lastDirection = direction;
    return direction * multiplier;
}

/**
 * A two-state boolean, i.e. exactly what viz.mjs `detectSwitch` draws as a
 * switch. Kept on the same BOOL_OPTION test so a control cannot be drawn as a
 * switch but turned like a list (or the reverse).
 */
function isSwitchMeta(meta) {
    return isBooleanMeta(meta);
}

/**
 * Minimum still time before a knob can flip a two-way control again.
 *
 * The same number and the same rule as `TRIGGER_KNOB_GESTURE_GAP_MS` in
 * page_controller.mjs: ONE FLICK IS ONE GESTURE. A trigger fires once per
 * flick; a two-way flips once per flick. Both would otherwise act a dozen
 * times across a single twist of the encoder.
 *
 * It is a LATCH, not a rate limit, and that is the distinction the trigger
 * shipped wrong first: the stamp is the last DETENT, so every detent extends
 * the gesture and the clock only runs while the knob is STILL.
 */
export const TWO_WAY_GESTURE_GAP_MS = 270;

/**
 * A control with exactly TWO values and no travel between them.
 *
 * Both spellings count — an Off/On (or int 0..1) boolean, which is drawn as a
 * switch, and a two-way CHOICE like Mix/Reverb or Saw/Square, which is drawn
 * as an enum square. They behave identically under the hand even though they
 * are drawn differently, because the question a detent asks is the same one:
 * there is nowhere to go but the other value.
 *
 * A TRIGGER is excluded. It is a two-option enum on the wire (["—","Rnd!"])
 * and toggling it would write "do nothing" on every other flick — which for
 * euclidrum's rnd_preset is the write that destroys a kit. Callers that route
 * triggers away before reaching here (page_controller does) are unaffected;
 * this is for the ones that do not.
 */
function isTwoWayMeta(meta) {
    if (!meta) return false;
    if (meta.writeOnly || meta.access === "write") return false;
    if (isSwitchMeta(meta)) return true;
    return (meta.kind === "enum" || meta.type === "enum")
        && Array.isArray(meta.options) && meta.options.length === 2;
}

/**
 * Reversing direction drops whatever partial turn was banked the other way.
 * Without it the residue is spent cancelling the new direction first, so a
 * reversal costs up to div + (div - 1) detents — 7 at ENUM_DELTA_DIV 4 — and
 * how many depends on where the previous turn happened to stop, which is why
 * the same knob feels inconsistent from one reversal to the next.
 */
function clearOnReversal(state, delta) {
    if (state.detentAccum !== 0 && Math.sign(state.detentAccum) !== Math.sign(delta))
        state.detentAccum = 0;
}

export function knobInit(initialValue) {
    return { value: initialValue, detentAccum: 0, lastTurnMs: 0, lastDirection: 0 };
}

/**
 * @param {object} state   from movyKnobInit, mutated in place
 * @param {object} meta    param_meta.mjs metadata (min/max/step/type/options)
 * @param {number} delta   ±1 per physical detent (this library always decodes
 *                         one CC message to one detent — see page_input.mjs)
 * @param {number} nowMs
 * @param {boolean} [fine] Shift-held precision mode. Not part of Movy's own
 *                 model (no equivalent was found in its source); grafted on
 *                 so the gesture works everywhere.
 *
 *                 THE RULE, in two clauses, both of which matter:
 *
 *                   1. the step becomes a TENTH of the coarse step, floored
 *                      at one whole unit for an int (a tenth of a 1-unit step
 *                      is 0.1, which rounds straight back — that was an int
 *                      knob that could not be moved at all under shift); and
 *                   2. the detent GATE is lifted, so every detent moves
 *                      something.
 *
 *                 Clause 2 is why shift can make a control FASTER rather than
 *                 slower, which looks contradictory on a stopwatch: an enum
 *                 and a narrow int are gated at ENUM_DELTA_DIV detents per
 *                 option in the coarse feel, so lifting the gate is a
 *                 fourfold speed-up. It is still precision — with shift held
 *                 one click is exactly one option, instead of four clicks
 *                 being one option and a partial turn being nothing at all.
 *                 That is what you want when placing a value rather than
 *                 sweeping to one.
 * @returns {number} the new value
 */
export function knobStep(state, meta, delta, nowMs, fine = false) {
    if (!state || !delta) return state ? state.value : 0;

    /*
     * Fill in a missing range. Call sites hand us metadata straight off
     * chain_params, and a module is not obliged to declare min/max -- an
     * absent one used to arrive as undefined, and the clamp at the bottom
     * turned the value into NaN, which is then written to the device as the
     * string "NaN". The old config-shaped entry point defaulted these on the
     * way in; now that there is only one entry point, it has to do it here.
     */
    if (typeof meta.min !== "number" || typeof meta.max !== "number") {
        const isInt = meta.type === "int";
        meta = {
            ...meta,
            min: typeof meta.min === "number" ? meta.min : 0,
            max: typeof meta.max === "number" ? meta.max : (isInt ? 127 : 1),
        };
    }

    /*
     * TWO VALUES: A DETENT TOGGLES, WHICHEVER WAY IT WENT.
     *
     * It used to be direction-ABSOLUTE — right meant option 1, left meant
     * option 0 — and a two-way choice like Mix/Reverb instead fell through to
     * the enum branch below and CLAMPED behind a four-detent gate. Three
     * spellings of one control, two of them with a dead direction:
     *
     *   Off/On at Off, turned left     nothing, forever
     *   Mix/Reverb at Mix, turned left nothing, forever
     *
     * Reported from the device: "if there are only two, why not let it wrap
     * otherwise you have to know which way is off and which way is on, in
     * which case you need some knowledge you dont have." There is no way to
     * acquire that knowledge from the screen — the cell shows a state, not a
     * direction — so half of every reach for the knob reads as a dead control.
     * That is the same argument that makes a trigger fire in either direction.
     *
     * WRAPPING ALONE WOULD NOT DO, and this is the part worth keeping. With
     * two values, "wrap" and "toggle on every detent" are the same thing, and
     * one flick of an encoder is a dozen detents — so a flick would land on
     * whichever value the detent count happened to be even or odd about. The
     * LATCH is what makes the gesture legible: one flick, one flip, and the
     * clock runs on STILLNESS because the stamp is the last detent rather than
     * the last flip.
     *
     * Hoisted above the enum branch for the reason it always was: an int 0..1
     * never enters that branch and would accumulate on the numeric path.
     */
    if (isTwoWayMeta(meta)) {
        const t = typeof nowMs === "number" ? nowMs : 0;
        const last = state.lastTwoWayMs;
        const startsGesture = last === undefined
            || (t - last) >= TWO_WAY_GESTURE_GAP_MS;
        state.lastTwoWayMs = t;
        state.detentAccum = 0;
        if (!startsGesture) return state.value;
        state.value = Math.round(Number(state.value)) === 0 ? 1 : 0;
        return state.value;
    }

    if (meta.kind === "enum" || meta.type === "enum") {
        /* THREE OR MORE options only — anything with exactly two was taken by
         * the branch above. That is what the four-detent gate is for: a long
         * list wants a sweep, and a two-way has nothing to sweep through, so
         * running one through this gate cost 4 detents to move at all (and up
         * to 7 to come back), which reads as a dead control — the drawn knob
         * simply does not move when you turn it. */
        const div = fine ? 1 : ENUM_DELTA_DIV;
        clearOnReversal(state, delta);
        state.detentAccum += delta;
        const steps = Math.trunc(state.detentAccum / div);
        if (steps === 0) return state.value;
        state.detentAccum -= steps * div;
        const count = Array.isArray(meta.options) ? meta.options.length : (meta.max - meta.min + 1);
        let iv = Math.round(state.value) + steps;
        iv = Math.max(0, Math.min(Math.max(0, count - 1), iv));
        state.value = iv;
        return state.value;
    }

    if (meta.knobAcceleration === "wide") {
        const scaled = wideStepCount(state, delta > 0 ? 1 : -1, nowMs) * (meta.step > 0 ? meta.step : 1);
        let next = state.value + scaled;
        next = Math.max(meta.min, Math.min(meta.max, next));
        if (meta.type === "int") next = Math.round(next);
        state.value = next;
        return next;
    }

    const div = fine ? 1 : detentsPerStep(meta);
    let steps = delta;
    if (div > 1) {
        clearOnReversal(state, delta);
        state.detentAccum += delta;
        steps = Math.trunc(state.detentAccum / div);
        if (steps === 0) return state.value;
        state.detentAccum -= steps * div;
    }
    /*
     * FINE is a tenth of the coarse step -- except an int, where a tenth of a
     * 1-unit step is 0.1 and Math.round below puts it straight back. That was
     * not "fine is subtle", it was an int knob that could not be moved AT ALL
     * with shift held: measured at 400,000 detents without crossing 0..127.
     * An int's finest meaningful move is 1.
     */
    const coarseStep = perDetentStep(meta);
    const stepSize = !fine ? coarseStep
        : (meta.type === "int" ? Math.max(1, Math.round(coarseStep * 0.1))
                               : coarseStep * 0.1);
    let next = state.value + steps * stepSize;
    next = Math.max(meta.min, Math.min(meta.max, next));
    if (meta.type === "int") next = Math.round(next);
    state.value = next;
    return next;
}

