/**
 * chain_ui_views.mjs - Reusable UI components for chain-related displays
 *
 * Shared between main chain UI and shadow UI for consistent rendering.
 */

/* Layout constants — RE-EXPORTED, not defined.
 *
 * This file used to carry a complete second copy of the chrome geometry, and
 * that copy was the OLD one (TITLE_Y 2, TITLE_RULE_Y 12, LIST_TOP_Y 15,
 * LIST_LABEL_X 4). Every shadow view module imports LIST_TOP_Y from HERE and
 * hands it back to drawMenuList as `listArea: { topY: LIST_TOP_Y }`, so those
 * ~20 call sites silently overrode menu_layout's re-skinned default and the
 * new chrome never reached the device: an 8-row dead band under the header and
 * four list rows instead of five.
 *
 * The names stay so the ~7 consumers need no edit; the values now come from the
 * one definition. This file cannot import them from menu_layout.mjs — that
 * module imports truncateText from here, so it would be a cycle. The leaf sits
 * below both, which is why it has no imports of its own. */
import {
    SCREEN_WIDTH, SCREEN_HEIGHT,
    TITLE_Y, TITLE_RULE_Y,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_TEXT_Y, FOOTER_RULE_Y,
} from './list_geometry.mjs';
export {
    SCREEN_WIDTH, SCREEN_HEIGHT,
    TITLE_Y, TITLE_RULE_Y,
    LIST_TOP_Y, LIST_LINE_HEIGHT, LIST_HIGHLIGHT_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_TEXT_Y, FOOTER_RULE_Y,
};

/* Parameter type constants */
export const PARAM_TYPE = {
    INT: "int",
    FLOAT: "float",
    ENUM: "enum",
    STRING: "string"
};

/**
 * Truncate text to fit within a maximum character count
 * @param {string} text - Text to truncate
 * @param {number} maxChars - Maximum character count
 * @returns {string} Truncated text with ellipsis if needed
 */
export function truncateText(text, maxChars) {
    if (!text) return "";
    if (text.length <= maxChars) return text;
    if (maxChars <= 3) return text.slice(0, maxChars);
    return `${text.slice(0, maxChars - 3)}...`;
}

/**
 * Format a parameter value for display
 * @param {string} type - Parameter type (int, float, enum, string)
 * @param {string|number} value - Parameter value
 * @param {number} [decimals=2] - Decimal places for float values
 * @returns {string} Formatted value string
 */
export function formatParamValue(type, value, decimals = 2) {
    if (value === null || value === undefined) return "-";

    switch (type) {
        case PARAM_TYPE.FLOAT:
            const num = parseFloat(value);
            if (isNaN(num)) return String(value);
            return num.toFixed(decimals);

        case PARAM_TYPE.INT:
            const int = parseInt(value);
            if (isNaN(int)) return String(value);
            return String(int);

        case PARAM_TYPE.ENUM:
        case PARAM_TYPE.STRING:
        default:
            return String(value);
    }
}

/**
 * Calculate adjusted value based on delta and constraints
 * @param {string} type - Parameter type
 * @param {string|number} currentValue - Current value
 * @param {number} delta - Change amount (+1 or -1)
 * @param {number} min - Minimum allowed value
 * @param {number} max - Maximum allowed value
 * @param {number} [step] - Step size (default: 1 for int, 0.05 for float)
 * @returns {string} New value as string
 */
export function adjustValue(type, currentValue, delta, min, max, step) {
    let val;

    if (type === PARAM_TYPE.FLOAT) {
        val = parseFloat(currentValue) || 0;
        const actualStep = step !== undefined ? step : 0.05;
        val += delta * actualStep;
    } else {
        val = parseInt(currentValue) || 0;
        const actualStep = step !== undefined ? step : 1;
        val += delta * actualStep;
    }

    /* Clamp to range */
    val = Math.max(min, Math.min(max, val));

    return formatParamValue(type, val);
}

