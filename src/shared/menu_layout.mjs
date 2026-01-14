/*
 * Shared menu layout helpers for title/list/footer screens.
 */

import { getMenuLabelScroller } from './text_scroll.mjs';

const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const TITLE_Y = 2;
const TITLE_RULE_Y = 12;
const LIST_TOP_Y = 15;
const LIST_LINE_HEIGHT = 11;
const LIST_HIGHLIGHT_HEIGHT = LIST_LINE_HEIGHT + 2;
const LIST_HIGHLIGHT_OFFSET = 0;
const LIST_LABEL_X = 4;
const LIST_VALUE_X = 75;
const LIST_MAX_VISIBLE = 5;
const LIST_INDICATOR_X = 120;
const LIST_INDICATOR_BOTTOM_Y = SCREEN_HEIGHT - 2;
const FOOTER_TEXT_Y = SCREEN_HEIGHT - 11;
const FOOTER_RULE_Y = FOOTER_TEXT_Y - 1;
const LIST_BOTTOM_WITH_FOOTER = FOOTER_RULE_Y - 1;
const DEFAULT_CHAR_WIDTH = 6;
const DEFAULT_LABEL_GAP = 6;
const DEFAULT_VALUE_PADDING_RIGHT = 2;

export function drawMenuHeader(title, titleRight = "") {
    print(2, TITLE_Y, title, 1);

    if (titleRight) {
        const rightX = SCREEN_WIDTH - (titleRight.length * DEFAULT_CHAR_WIDTH) - 2;
        print(Math.max(2, rightX), TITLE_Y, titleRight, 1);
    }

    fill_rect(0, TITLE_RULE_Y, SCREEN_WIDTH, 1, 1);
}

export function drawMenuFooter(text, y = FOOTER_TEXT_Y) {
    if (text) {
        fill_rect(0, FOOTER_RULE_Y, SCREEN_WIDTH, 1, 1);
        print(2, y, text, 1);
    }
}

function drawArrowUp(x, y) {
    set_pixel(x + 2, y, 1);
    set_pixel(x + 1, y + 1, 1);
    set_pixel(x + 3, y + 1, 1);
    for (let i = 0; i < 5; i++) {
        set_pixel(x + i, y + 2, 1);
    }
}

function drawArrowDown(x, y) {
    for (let i = 0; i < 5; i++) {
        set_pixel(x + i, y, 1);
    }
    set_pixel(x + 1, y + 1, 1);
    set_pixel(x + 3, y + 1, 1);
    set_pixel(x + 2, y + 2, 1);
}

export function drawMenuList({
    items,
    selectedIndex,
    listArea,
    topY = LIST_TOP_Y,
    lineHeight = LIST_LINE_HEIGHT,
    highlightHeight = LIST_HIGHLIGHT_HEIGHT,
    highlightOffset = LIST_HIGHLIGHT_OFFSET,
    labelX = LIST_LABEL_X,
    valueX = LIST_VALUE_X,
    valueAlignRight = false,
    valuePaddingRight = DEFAULT_VALUE_PADDING_RIGHT,
    labelGap = DEFAULT_LABEL_GAP,
    maxVisible = 0,
    keepOffLastRow = true,
    indicatorX = LIST_INDICATOR_X,
    indicatorBottomY = LIST_INDICATOR_BOTTOM_Y,
    getLabel,
    getValue
}) {
    const totalItems = items.length;
    const resolvedTopY = listArea?.topY ?? topY;
    const resolvedBottomY = listArea?.bottomY ?? indicatorBottomY;
    const computedMaxVisible = maxVisible > 0
        ? maxVisible
        : Math.max(1, Math.floor((resolvedBottomY - resolvedTopY) / lineHeight));
    const effectiveMaxVisible = computedMaxVisible;
    let startIdx = 0;

    const maxSelectedRow = keepOffLastRow
        ? effectiveMaxVisible - 2
        : effectiveMaxVisible - 1;
    if (selectedIndex > maxSelectedRow) {
        startIdx = selectedIndex - maxSelectedRow;
    }
    let endIdx = Math.min(startIdx + effectiveMaxVisible, totalItems);

    /* Get label scroller for selected item and tick it */
    const labelScroller = getMenuLabelScroller();
    labelScroller.setSelected(selectedIndex);
    labelScroller.tick();  /* Auto-tick during draw */

    for (let i = startIdx; i < endIdx; i++) {
        const y = resolvedTopY + (i - startIdx) * lineHeight;
        const item = items[i];
        const isSelected = i === selectedIndex;

        const labelPrefix = isSelected ? "> " : "  ";
        let label = getLabel(item, i);
        const fullLabel = label; /* Keep original for scrolling */
        const value = getValue ? getValue(item, i) : "";
        let resolvedValueX = valueX;
        let maxLabelChars = 0;

        if (valueAlignRight && value) {
            resolvedValueX = SCREEN_WIDTH - (value.length * DEFAULT_CHAR_WIDTH) - valuePaddingRight;
            const maxLabelWidth = Math.max(0, resolvedValueX - labelX - labelGap);
            maxLabelChars = Math.floor((maxLabelWidth - (labelPrefix.length * DEFAULT_CHAR_WIDTH)) / DEFAULT_CHAR_WIDTH);
        } else {
            /* No value, label can use full width minus prefix and indicator */
            const maxLabelWidth = indicatorX - labelX - labelGap;
            maxLabelChars = Math.floor((maxLabelWidth - (labelPrefix.length * DEFAULT_CHAR_WIDTH)) / DEFAULT_CHAR_WIDTH);
        }

        if (maxLabelChars > 0) {
            if (isSelected && fullLabel.length > maxLabelChars) {
                /* Selected item with long text: use scroller */
                label = labelScroller.getScrolledText(fullLabel, maxLabelChars);
            } else {
                /* Truncate non-selected or short text */
                label = truncateText(fullLabel, maxLabelChars);
            }
        }

        if (isSelected) {
            fill_rect(0, y - highlightOffset, SCREEN_WIDTH, highlightHeight, 1);
            print(labelX, y, `${labelPrefix}${label}`, 0);
            if (value) {
                print(resolvedValueX, y, value, 0);
            }
        } else {
            print(labelX, y, `${labelPrefix}${label}`, 1);
            if (value) {
                print(resolvedValueX, y, value, 1);
            }
        }
    }

    if (startIdx > 0) {
        drawArrowUp(indicatorX, resolvedTopY);
    }
    if (endIdx < totalItems) {
        drawArrowDown(indicatorX, resolvedBottomY - 2);
    }
}

export const menuLayoutDefaults = {
    footerY: FOOTER_TEXT_Y,
    listTopY: LIST_TOP_Y,
    listBottomWithFooter: LIST_BOTTOM_WITH_FOOTER,
    listBottomNoFooter: LIST_INDICATOR_BOTTOM_Y
};

function truncateText(text, maxChars) {
    if (text.length <= maxChars) return text;
    if (maxChars <= 3) return text.slice(0, maxChars);
    return `${text.slice(0, maxChars - 3)}...`;
}

/* === Parameter Overlay === */
/* A centered overlay for showing parameter name and value feedback */

const OVERLAY_DURATION_TICKS = 60;  /* ~1 second at 60fps */
const OVERLAY_WIDTH = 120;
const OVERLAY_HEIGHT = 28;

let overlayActive = false;
let overlayName = "";
let overlayValue = "";
let overlayTimeout = 0;

/**
 * Show the parameter overlay with a name and value
 * @param {string} name - Parameter name to display
 * @param {string} value - Value to display (e.g., "50%" or "3")
 */
export function showOverlay(name, value) {
    overlayActive = true;
    overlayName = name;
    overlayValue = value;
    overlayTimeout = OVERLAY_DURATION_TICKS;
}

/**
 * Hide the overlay immediately
 */
export function hideOverlay() {
    overlayActive = false;
    overlayTimeout = 0;
}

/**
 * Check if overlay is currently active
 * @returns {boolean}
 */
export function isOverlayActive() {
    return overlayActive;
}

/**
 * Tick the overlay timer - call this in your tick() function
 * @returns {boolean} true if overlay state changed (needs redraw)
 */
export function tickOverlay() {
    if (overlayTimeout > 0) {
        overlayTimeout--;
        if (overlayTimeout === 0) {
            overlayActive = false;
            return true;  /* State changed, needs redraw */
        }
    }
    return false;
}

/**
 * Draw the overlay if active - call this at the end of your draw function
 */
export function drawOverlay() {
    if (!overlayActive || !overlayName) return;

    const boxX = (SCREEN_WIDTH - OVERLAY_WIDTH) / 2;
    const boxY = (SCREEN_HEIGHT - OVERLAY_HEIGHT) / 2;

    /* Background and border */
    fill_rect(boxX, boxY, OVERLAY_WIDTH, OVERLAY_HEIGHT, 0);  /* Clear background */
    fill_rect(boxX, boxY, OVERLAY_WIDTH, 1, 1);     /* Top border */
    fill_rect(boxX, boxY + OVERLAY_HEIGHT - 1, OVERLAY_WIDTH, 1, 1);  /* Bottom border */
    fill_rect(boxX, boxY, 1, OVERLAY_HEIGHT, 1);     /* Left border */
    fill_rect(boxX + OVERLAY_WIDTH - 1, boxY, 1, OVERLAY_HEIGHT, 1);  /* Right border */

    /* Parameter name and value */
    const displayName = overlayName.length > 18 ? overlayName.substring(0, 18) : overlayName;
    print(boxX + 4, boxY + 2, displayName, 1);
    print(boxX + 4, boxY + 14, `Value: ${overlayValue}`, 1);
}

/* === Status Overlay === */
/* A centered overlay for status/loading messages */

const STATUS_OVERLAY_WIDTH = 120;
const STATUS_OVERLAY_HEIGHT = 40;

/**
 * Draw a centered status overlay with title and message
 * Used for loading states, installation progress, etc.
 * @param {string} title - Title text (e.g., "Installing")
 * @param {string} message - Message text (e.g., "JV-880 v0.2.0")
 */
export function drawStatusOverlay(title, message) {
    const boxX = (SCREEN_WIDTH - STATUS_OVERLAY_WIDTH) / 2;
    const boxY = (SCREEN_HEIGHT - STATUS_OVERLAY_HEIGHT) / 2;

    /* Background and double border */
    fill_rect(boxX, boxY, STATUS_OVERLAY_WIDTH, STATUS_OVERLAY_HEIGHT, 0);
    draw_rect(boxX, boxY, STATUS_OVERLAY_WIDTH, STATUS_OVERLAY_HEIGHT, 1);
    draw_rect(boxX + 1, boxY + 1, STATUS_OVERLAY_WIDTH - 2, STATUS_OVERLAY_HEIGHT - 2, 1);

    /* Center title */
    const titleW = title.length * 6;
    print(Math.floor((SCREEN_WIDTH - titleW) / 2), boxY + 10, title, 1);

    /* Center message */
    const msgW = message.length * 6;
    print(Math.floor((SCREEN_WIDTH - msgW) / 2), boxY + 24, message, 1);
}

/* Note: Label scroller is auto-ticked inside drawMenuList() */
