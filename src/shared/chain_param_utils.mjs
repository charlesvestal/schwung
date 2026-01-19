/**
 * chain_param_utils.mjs - Utilities for chain parameter routing
 *
 * Shared between main chain UI and shadow UI for consistent parameter handling.
 */

/* Valid component prefixes */
export const COMPONENT_PREFIXES = ["synth", "fx1", "fx2", "source", "midi_fx"];

/**
 * Parse a parameter key with prefix
 * Examples:
 *   "synth:preset" -> { target: "synth", param: "preset" }
 *   "fx1:wet" -> { target: "fx1", param: "wet" }
 *   "volume" -> null (no prefix)
 *
 * @param {string} key - Full parameter key
 * @returns {object|null} Parsed key or null if no prefix
 */
export function parseParamKey(key) {
    if (!key || typeof key !== "string") return null;

    const colonIndex = key.indexOf(":");
    if (colonIndex === -1) return null;

    const target = key.slice(0, colonIndex);
    const param = key.slice(colonIndex + 1);

    if (!target || !param) return null;
    if (!COMPONENT_PREFIXES.includes(target)) return null;

    return { target, param };
}

/**
 * Build a prefixed parameter key
 * @param {string} target - Component target (synth, fx1, fx2, etc.)
 * @param {string} param - Parameter name
 * @returns {string} Full parameter key
 */
export function buildParamKey(target, param) {
    return `${target}:${param}`;
}

/**
 * Strip the prefix from a parameter key
 * @param {string} key - Full parameter key
 * @returns {string} Parameter name without prefix
 */
export function stripParamPrefix(key) {
    const parsed = parseParamKey(key);
    return parsed ? parsed.param : key;
}

/**
 * Get the target component from a parameter key
 * @param {string} key - Full parameter key
 * @returns {string|null} Target component or null
 */
export function getParamTarget(key) {
    const parsed = parseParamKey(key);
    return parsed ? parsed.target : null;
}

/**
 * Check if a parameter key has a specific prefix
 * @param {string} key - Full parameter key
 * @param {string} prefix - Expected prefix
 * @returns {boolean} True if key has the specified prefix
 */
export function hasParamPrefix(key, prefix) {
    if (!key || typeof key !== "string") return false;
    return key.startsWith(prefix + ":");
}

/**
 * Group parameters by their target component
 * @param {Array} params - Array of {key, value} objects
 * @returns {object} Object with target names as keys, arrays of params as values
 */
export function groupParamsByTarget(params) {
    const groups = {};

    for (const param of params) {
        const parsed = parseParamKey(param.key);
        const target = parsed ? parsed.target : "global";

        if (!groups[target]) {
            groups[target] = [];
        }
        groups[target].push({
            ...param,
            localKey: parsed ? parsed.param : param.key
        });
    }

    return groups;
}

/**
 * Common synth parameters that most synth modules support
 */
export const COMMON_SYNTH_PARAMS = [
    { key: "preset", label: "Preset", type: "int", min: 0, max: 127 },
    { key: "volume", label: "Volume", type: "float", min: 0, max: 1 },
    { key: "pan", label: "Pan", type: "float", min: -1, max: 1 },
    { key: "transpose", label: "Transpose", type: "int", min: -24, max: 24 },
    { key: "tune", label: "Tune", type: "float", min: -1, max: 1 },
];

/**
 * Common audio FX parameters that most FX modules support
 */
export const COMMON_FX_PARAMS = [
    { key: "wet", label: "Wet", type: "float", min: 0, max: 1 },
    { key: "dry", label: "Dry", type: "float", min: 0, max: 1 },
    { key: "mix", label: "Mix", type: "float", min: 0, max: 1 },
    { key: "bypass", label: "Bypass", type: "int", min: 0, max: 1 },
];

/**
 * Reverb-specific parameters
 */
export const REVERB_PARAMS = [
    { key: "room_size", label: "Room Size", type: "float", min: 0, max: 1 },
    { key: "damping", label: "Damping", type: "float", min: 0, max: 1 },
    { key: "width", label: "Width", type: "float", min: 0, max: 1 },
    { key: "predelay", label: "Predelay", type: "float", min: 0, max: 1 },
    { key: "decay", label: "Decay", type: "float", min: 0, max: 1 },
];

/**
 * Delay-specific parameters
 */
export const DELAY_PARAMS = [
    { key: "time", label: "Time", type: "float", min: 0, max: 2 },
    { key: "feedback", label: "Feedback", type: "float", min: 0, max: 1 },
    { key: "sync", label: "Sync", type: "int", min: 0, max: 1 },
];

/**
 * Get parameter definitions for a component type
 * @param {string} componentType - Type of component (synth, reverb, delay, etc.)
 * @returns {Array} Array of parameter definitions
 */
export function getParamDefsForComponent(componentType) {
    switch (componentType) {
        case "synth":
            return COMMON_SYNTH_PARAMS;
        case "reverb":
            return [...COMMON_FX_PARAMS, ...REVERB_PARAMS];
        case "delay":
            return [...COMMON_FX_PARAMS, ...DELAY_PARAMS];
        default:
            return COMMON_FX_PARAMS;
    }
}
