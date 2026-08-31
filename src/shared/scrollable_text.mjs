/*
 * Scrollable Text Component
 *
 * Displays multi-line text that scrolls with jog wheel, with a fixed
 * action button at the bottom that becomes selected after scrolling
 * past all text.
 */

import { drawScrollbar, LIST_LINE_HEIGHT } from './menu_layout.mjs';

const SCREEN_WIDTH = 128;
const CHAR_WIDTH = 6;
/*
 * THE SAME ROW PITCH AS EVERY LIST, and that is worth a whole line.
 *
 * This was 10px against the shared list's 9. Both draw the same 5x7 font into
 * the same 10..55 rect, so the list fitted FIVE rows there and the text fitted
 * four — one line of help thrown away, per screen, to a constant nobody had a
 * reason for. Reported from hardware: "i feel like our layout is wrong and we
 * can fit an extra line in?"
 *
 * Derived, not re-typed at 9: the pitch and the row count are computed from the
 * same two numbers `drawMenuList` uses (see visibleLinesFor), so text and list
 * cannot drift apart again.
 */
const LINE_HEIGHT = LIST_LINE_HEIGHT;
const MAX_CHARS_PER_LINE = 20;
/* The device 5x7 font's ink height, against LINE_HEIGHT's pitch — the scrollbar
 * track measures ROWS OF INK, not the leading under the last one. */
const GLYPH_INK_HEIGHT = 7;

/**
 * How many lines fit between topY and bottomY — drawMenuList's own formula, so
 * a text area and a list occupying the same rect always agree on the row count.
 *
 * Callers pass the result as `visibleLines` rather than counting rows by hand:
 * a hard-coded 4 is what silently lost the fifth line when the pitch changed,
 * and would lose it again the next time the rect moves.
 *
 * @param {number} topY     first line's y
 * @param {number} bottomY  the floor (a footer rule, an action button)
 */
export function visibleLinesFor(topY, bottomY) {
    return Math.max(1, Math.floor((bottomY - topY) / LINE_HEIGHT));
}

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
export function createScrollableText({ lines, actionLabel, visibleLines = 4, onActionSelected }) {
    return {
        lines: lines || [],
        actionLabel: actionLabel || 'OK',
        visibleLines,
        scrollOffset: 0,
        actionSelected: false,
        onActionSelected: onActionSelected || null
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
            if (state.onActionSelected) {
                state.onActionSelected(state.actionLabel);
            }
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

    /*
     * THE SAME BAR EVERY LIST DRAWS, not the arrows this used to draw.
     *
     * This was the last surface in the shadow UI still using drawArrowUp /
     * drawArrowDown, and the help viewer is where that showed: its topic LIST
     * (drawMenuList) wore a scrollbar and the help TEXT one click further in
     * wore arrows, in one session, on one jog. Reported from hardware as "we
     * are using the wrong scrollbars".
     *
     * The window is FIXED at visibleLines, so `visible` is that whether or not
     * the last screenful is short — handleScrollableTextJog clamps scrollOffset
     * to lines.length - visibleLines, so a partial last window cannot happen.
     *
     * `rowInk` is 7: the device 5x7 font's glyph height, against LINE_HEIGHT's
     * 10px pitch. It is what stops the track overhanging the last line of text
     * by the 3px of leading underneath it.
     */
    drawScrollbar({
        topY,
        bottomY,
        rowHeight: LINE_HEIGHT,
        rowInk: GLYPH_INK_HEIGHT,
        windowRows: visibleLines,
        total: lines.length,
        startIdx: scrollOffset,
    });

    /* Draw action button (skip if actionY < 0) */
    if (actionY >= 0) {
        fill_rect(0, actionY - 6, SCREEN_WIDTH, 1, 1);

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
}

