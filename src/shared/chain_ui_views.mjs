/**
 * chain_ui_views.mjs - Reusable UI components for chain-related displays
 *
 * Shared between main chain UI and shadow UI for consistent rendering.
 */

/* Layout constants */
export const SCREEN_WIDTH = 128;
export const SCREEN_HEIGHT = 64;
export const TITLE_Y = 2;
export const TITLE_RULE_Y = 12;
export const LIST_TOP_Y = 15;
export const LIST_LINE_HEIGHT = 9;
export const LIST_HIGHLIGHT_HEIGHT = LIST_LINE_HEIGHT;
export const LIST_LABEL_X = 4;
export const LIST_VALUE_X = 92;
export const FOOTER_TEXT_Y = SCREEN_HEIGHT - 7;
export const FOOTER_RULE_Y = FOOTER_TEXT_Y - 2;

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

/**
 * Calculate visible range for scrolling list
 * @param {number} itemCount - Total number of items
 * @param {number} selectedIndex - Currently selected index
 * @param {number} maxVisible - Maximum visible items
 * @returns {{startIdx: number, endIdx: number}} Visible range
 */
export function calculateVisibleRange(itemCount, selectedIndex, maxVisible) {
    let startIdx = 0;
    const maxSelectedRow = maxVisible - 2;

    if (selectedIndex > maxSelectedRow) {
        startIdx = selectedIndex - maxSelectedRow;
    }

    const endIdx = Math.min(startIdx + maxVisible, itemCount);

    return { startIdx, endIdx };
}

/**
 * Create a select list renderer function
 * This returns a function that can be used with different rendering contexts
 *
 * @param {object} ctx - Rendering context with print, fill_rect functions
 * @returns {Function} Renderer function
 */
export function createSelectListRenderer(ctx) {
    return function drawSelectList(items, selectedIndex, getLabel, getValue, options = {}) {
        const {
            topY = LIST_TOP_Y,
            lineHeight = LIST_LINE_HEIGHT,
            highlightHeight = LIST_HIGHLIGHT_HEIGHT,
            labelX = LIST_LABEL_X,
            valueX = LIST_VALUE_X,
            footerY = FOOTER_RULE_Y
        } = options;

        const maxVisible = Math.max(1, Math.floor((footerY - topY) / lineHeight));
        const { startIdx, endIdx } = calculateVisibleRange(items.length, selectedIndex, maxVisible);
        const maxLabelChars = Math.floor((valueX - labelX - 6) / 6);

        for (let i = startIdx; i < endIdx; i++) {
            const y = topY + (i - startIdx) * lineHeight;
            const item = items[i];
            const label = truncateText(getLabel(item, i), maxLabelChars);
            const value = getValue ? getValue(item, i) : "";
            const isSelected = i === selectedIndex;

            if (isSelected) {
                ctx.fill_rect(0, y - 1, SCREEN_WIDTH, highlightHeight, 1);
                ctx.print(labelX, y, `> ${label}`, 0);
                if (value) {
                    ctx.print(valueX, y, value, 0);
                }
            } else {
                ctx.print(labelX, y, `  ${label}`, 1);
                if (value) {
                    ctx.print(valueX, y, value, 1);
                }
            }
        }
    };
}
