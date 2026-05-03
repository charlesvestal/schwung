/**
 * knob_engine.mjs — unified knob acceleration + value-stepping.
 * Port of schwung-rewrite/src/domains/knob_engine.c into JS.
 *
 * Same divisor curve used in every chain/master-FX/slot param edit:
 *   gap < 50ms   → divisor 4   (fast sweep)
 *   gap < 150ms  → divisor 8
 *   gap >= 150ms → divisor 16  (fine control)
 *
 * Float: step / divisor per tick.
 * Int:   accumulate ticks; emit ±1 once accum reaches divisor.
 * Enum:  enum_divisor = clamp(800/enumCount, 2, 40); accumulate; emit ±1.
 *        (Equates "full enum sweep" to "full float 0→1 sweep" in physical effort.)
 */

export const KNOB_TYPE_FLOAT = "float";
export const KNOB_TYPE_INT = "int";
export const KNOB_TYPE_ENUM = "enum";

const KNOB_ACCEL_FAST_MS = 50;
const KNOB_ACCEL_MED_MS = 150;

export function knobInit(initialValue) {
    return { lastTickMs: 0, value: initialValue, tickAccum: 0 };
}

function clampf(v, lo, hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

function tickDivisor(state, nowMs) {
    if (state.lastTickMs === 0 || nowMs <= state.lastTickMs) return 1;
    const delta = nowMs - state.lastTickMs;
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
        state.tickAccum += direction;
        if (state.tickAccum >= divisor || state.tickAccum <= -divisor) {
            const sign = state.tickAccum > 0 ? 1 : -1;
            state.value = clampf(state.value + sign, config.min, config.max);
            state.tickAccum = 0;
        }
    } else if (config.type === KNOB_TYPE_ENUM) {
        let enumDivisor = divisor;
        if (config.enumCount && config.enumCount > 0) {
            let perOption = Math.floor(800 / config.enumCount);
            if (perOption < 2) perOption = 2;
            if (perOption > 40) perOption = 40;
            enumDivisor = perOption;
        }
        state.tickAccum += direction;
        if (state.tickAccum >= enumDivisor || state.tickAccum <= -enumDivisor) {
            const sign = state.tickAccum > 0 ? 1 : -1;
            let iv = Math.round(state.value) + sign;
            if (iv < 0) iv = 0;
            if (config.enumCount && iv >= config.enumCount) iv = config.enumCount - 1;
            state.value = iv;
            state.tickAccum = 0;
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
