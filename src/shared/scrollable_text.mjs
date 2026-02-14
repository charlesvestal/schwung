/*
 * Scrollable Text Component
 *
 * Displays multi-line text that scrolls with jog wheel, with a fixed
 * action button at the bottom that becomes selected after scrolling
 * past all text.
 */

import { drawArrowUp, drawArrowDown } from './menu_layout.mjs';

const SCREEN_WIDTH = 128;
const CHAR_WIDTH = 6;
const LINE_HEIGHT = 10;
const MAX_CHARS_PER_LINE = 20;

/**
 * Word-wrap text into lines
 * @param {string} text - Text to wrap
 * @param {number} maxChars - Max characters per line
 * @returns {string[]} Array of lines
 */
export function wrapText(text, maxChars = MAX_CHARS_PER_LINE) {
    if (!text) return [];

    const words = text.split(/\s+/);
    const lines = [];
    let currentLine = '';

    for (const word of words) {
        if (currentLine.length === 0) {
            currentLine = word;
        } else if (currentLine.length + 1 + word.length <= maxChars) {
            currentLine += ' ' + word;
        } else {
            lines.push(currentLine);
            currentLine = word;
        }
    }
    if (currentLine) {
        lines.push(currentLine);
    }

    return lines;
}

/**
 * Create scrollable text state
 * @param {Object} options
 * @param {string[]} options.lines - Pre-wrapped text lines
 * @param {string} options.actionLabel - Label for action button (e.g., "Install")
 * @param {number} options.visibleLines - Number of visible lines (default 4)
 * @returns {Object} State object
 */
export function createScrollableText({ lines, actionLabel, visibleLines = 4 }) {
    return {
        lines: lines || [],
        actionLabel: actionLabel || 'OK',
        visibleLines,
        scrollOffset: 0,
        actionSelected: false
    };
}

/**
 * Handle jog input for scrollable text
 * @param {Object} state - Scrollable text state
 * @param {number} delta - Jog delta (-1 or 1)
 * @returns {boolean} true if state changed
 */
export function handleScrollableTextJog(state, delta) {
    const maxScroll = Math.max(0, state.lines.length - state.visibleLines);

    if (delta > 0) {
        /* Scroll down */
        if (state.actionSelected) {
            return false; /* Already at bottom */
        }
        if (state.scrollOffset >= maxScroll) {
            /* At end of text, select action */
            state.actionSelected = true;
            return true;
        }
        state.scrollOffset++;
        return true;
    } else if (delta < 0) {
        /* Scroll up */
        if (state.actionSelected) {
            state.actionSelected = false;
            return true;
        }
        if (state.scrollOffset > 0) {
            state.scrollOffset--;
            return true;
        }
    }
    return false;
}

/**
 * Check if action is selected
 * @param {Object} state - Scrollable text state
 * @returns {boolean}
 */
export function isActionSelected(state) {
    return state.actionSelected;
}

/**
 * Draw scrollable text area
 * @param {Object} options
 * @param {Object} options.state - Scrollable text state
 * @param {number} options.topY - Top Y position of text area
 * @param {number} options.bottomY - Bottom Y position (above action button)
 * @param {number} options.actionY - Y position of action button
 */
export function drawScrollableText({ state, topY, bottomY, actionY }) {
    const { lines, scrollOffset, actionSelected, actionLabel, visibleLines } = state;

    /* Draw visible text lines */
    const endIdx = Math.min(scrollOffset + visibleLines, lines.length);
    for (let i = scrollOffset; i < endIdx; i++) {
        const y = topY + (i - scrollOffset) * LINE_HEIGHT;
        print(4, y, lines[i], 1);
    }

    /* Draw scroll indicators */
    const indicatorX = 122;
    if (scrollOffset > 0) {
        /* Up arrow */
        drawArrowUp(indicatorX, topY);
    }
    const maxScroll = Math.max(0, lines.length - visibleLines);
    if (scrollOffset < maxScroll) {
        /* Down arrow - more text below */
        drawArrowDown(indicatorX, bottomY - 4);
    }

    /* Draw divider above action button */
    fill_rect(0, actionY - 6, SCREEN_WIDTH, 1, 1);

    /* Draw action button */
    const buttonText = `[${actionLabel}]`;
    const buttonWidth = buttonText.length * CHAR_WIDTH + 8;
    const buttonX = (SCREEN_WIDTH - buttonWidth) / 2;

    if (actionSelected) {
        fill_rect(buttonX - 2, actionY - 2, buttonWidth + 4, 14, 1);
        print(buttonX + 4, actionY, buttonText, 0);
    } else {
        print(buttonX + 4, actionY, buttonText, 1);
    }
}

