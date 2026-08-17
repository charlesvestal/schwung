/**
 * movy_knob.mjs — schwung-movy's own knob-delta math, ported
 * (`src/model/{knob-step,store,constants}.ts`, © 2026 megadake, MIT —
 * https://github.com/DimaDake/schwung-movy), not Schwung's pre-existing
 * time-based acceleration (`../knob_engine.mjs`, which predates this port and
 * is a different feel entirely — ported from schwung-rewrite's C host, not
 * from Movy). Used only when `page_controller.mjs`'s layout is LAYOUT_MOVY,
 * so the Movy-style knob grid also turns like Movy, not like Schwung's own
 * dial/bar grid.
 *
 * The core idea: a float/int knob's per-detent step is normalized to a fixed
 * FRACTION of its own range (~1%, MIN_STEP_RANGE_FRAC) rather than using
 * the module's declared `step` directly — every knob gets roughly the same
 * ~100-detent sweep regardless of units, which is what fixes both a
 * wide-range knob crawling and a narrow one being a hair trigger. There is
 * no time/speed-based acceleration for an ordinary knob — Movy's "wide"
 * per-param opt-in (a velocity multiplier table) is ported for fidelity, but
 * nothing in this library's contract declares `knobAcceleration: "wide"`
 * today, so it never fires; see the module doc in viz_draw.mjs for the same
 * kind of "ported but currently inert" note.
 */

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

export function movyKnobInit(initialValue) {
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
 *                 the same way Schwung's existing knob_engine.mjs offers it,
 *                 so the gesture keeps working under the Movy layout too —
 *                 bypasses the enum/narrow-int click gate and runs the step
 *                 at a tenth size, mirroring page_controller.mjs's own
 *                 `canRefine` handling for the other engine.
 * @returns {number} the new value
 */
export function movyKnobTick(state, meta, delta, nowMs, fine = false) {
    if (delta === 0) return state.value;

    if (meta.kind === "enum" || meta.type === "enum") {
        const div = fine ? 1 : ENUM_DELTA_DIV;
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
        state.detentAccum += delta;
        steps = Math.trunc(state.detentAccum / div);
        if (steps === 0) return state.value;
        state.detentAccum -= steps * div;
    }
    const stepSize = perDetentStep(meta) * (fine ? 0.1 : 1);
    let next = state.value + steps * stepSize;
    next = Math.max(meta.min, Math.min(meta.max, next));
    if (meta.type === "int") next = Math.round(next);
    state.value = next;
    return next;
}
