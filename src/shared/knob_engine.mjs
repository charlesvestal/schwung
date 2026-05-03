/**
 * knob_engine.mjs — unified knob acceleration + value-stepping.
 * Port of schwung-rewrite/src/domains/knob_engine.c into JS.
 *
 * Divisor curve (gap = nowMs - lastTickMs):
 *   gap >  150ms → divisor 16   (fine control)
 *   gap >   50ms → divisor 8
 *   gap == 0 or <= 50ms → divisor 4   (fast sweep)
 *   first tick (lastTickMs == 0) → divisor 1   (intentional "click" on motion start)
 *
 * Float: step / divisor per tick.
 * Int:   accumulate ticks; emit ±1 once accum reaches divisor.
 * Enum:  enum_divisor = clamp(2100/enumCount, 6, 100); accumulate; emit ±1.
 *        (Tuned so a fast wrist sweep covers a 47-option list end-to-end —
 *        ~2100 ticks for full sweep, ~44 ticks per option.)
 *
 * Staleness: gap > 2000ms resets the engine to cold-start (lastTickMs=0),
 * so re-entering an editor after a long pause feels like a fresh edit
 * rather than continuing a stale acceleration curve.
 *
 * Batched deltas: |direction| > 1 produces proportional motion. The
 * accumulator emits floor(|accum|/divisor) steps per call (int/enum) or
 * step*direction/divisor (float), preserving the old linear "delta * step"
 * behavior under fast jog wheel input.
 */

export const KNOB_TYPE_FLOAT = "float";
export const KNOB_TYPE_INT = "int";
export const KNOB_TYPE_ENUM = "enum";

const KNOB_ACCEL_FAST_MS = 50;
const KNOB_ACCEL_MED_MS = 150;
const KNOB_STALE_MS = 2000;   // gap above this → treat as cold start (engine self-resets)

export function knobInit(initialValue) {
    return { lastTickMs: 0, value: initialValue, tickAccum: 0 };
}

function clampf(v, lo, hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

function tickDivisor(state, nowMs) {
    if (state.lastTickMs === 0) return 1;
    const delta = nowMs > state.lastTickMs ? nowMs - state.lastTickMs : 0;
    /* Stale state — engine self-resets so re-entry feels like a fresh edit. */
    if (delta > KNOB_STALE_MS) {
        state.lastTickMs = 0;
        state.tickAccum = 0;
        return 1;
    }
    if (delta > KNOB_ACCEL_MED_MS) return 16;
    if (delta > KNOB_ACCEL_FAST_MS) return 8;
    return 4;
}

export function knobTick(state, config, direction, nowMs) {
    const divisor = tickDivisor(state, nowMs);
    state.lastTickMs = nowMs;

    if (config.type === KNOB_TYPE_FLOAT) {
        const step = config.step > 0 ? config.step : 0.01;
        const delta = (step / divisor) * direction;
        state.value = clampf(state.value + delta, config.min, config.max);
    } else if (config.type === KNOB_TYPE_INT) {
        /* Accumulator must drain before reversing — eats first N reverse ticks (anti-jitter). */
        state.tickAccum += direction;
        const steps = Math.trunc(state.tickAccum / divisor);
        if (steps !== 0) {
            state.value = clampf(state.value + steps, config.min, config.max);
            state.tickAccum -= steps * divisor;
        }
    } else if (config.type === KNOB_TYPE_ENUM) {
        if (!config.enumCount || config.enumCount <= 0) {
            state.tickAccum = 0;
            return state.value;
        }
        let perOption = Math.floor(2100 / config.enumCount);
        if (perOption < 6) perOption = 6;
        if (perOption > 100) perOption = 100;
        const enumDivisor = perOption;
        /* Accumulator must drain before reversing — eats first N reverse ticks (anti-jitter). */
        state.tickAccum += direction;
        const steps = Math.trunc(state.tickAccum / enumDivisor);
        if (steps !== 0) {
            let iv = Math.round(state.value) + steps;
            if (iv < 0) iv = 0;
            if (iv >= config.enumCount) iv = config.enumCount - 1;
            state.value = iv;
            state.tickAccum -= steps * enumDivisor;
        }
    }
    return state.value;
}

/* Convert a chain_params metadata entry → KnobConfig accepted by knobTick(). */
export function knobConfigFromMeta(meta) {
    if (!meta) return { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 };
    if (meta.type === "int") {
        return {
            type: KNOB_TYPE_INT,
            min: typeof meta.min === "number" ? meta.min : 0,
            max: typeof meta.max === "number" ? meta.max : 127,
            step: meta.step > 0 ? meta.step : 1,
        };
    }
    if (meta.type === "enum") {
        const opts = Array.isArray(meta.options) ? meta.options : [];
        return {
            type: KNOB_TYPE_ENUM,
            min: 0,
            max: Math.max(0, opts.length - 1),
            step: 1,
            enumCount: opts.length,
        };
    }
    return {
        type: KNOB_TYPE_FLOAT,
        min: typeof meta.min === "number" ? meta.min : 0,
        max: typeof meta.max === "number" ? meta.max : 1,
        step: meta.step > 0 ? meta.step : 0.01,
    };
}
