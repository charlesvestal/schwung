/*
 * Shared text entry component - on-screen keyboard for text input
 *
 * Usage:
 *   import { openTextEntry, isTextEntryActive, handleTextEntryMidi, drawTextEntry, tickTextEntry } from './text_entry.mjs';
 *
 *   openTextEntry({
 *       title: "Rename Patch",
 *       initialText: "My Patch",
 *       onConfirm: (text) => { ... },
 *       onCancel: () => { ... }
 *   });
 *
 *   // In your onMidiMessage:
 *   if (isTextEntryActive()) { handleTextEntryMidi(msg); return; }
 *
 *   // In your tick:
 *   if (isTextEntryActive()) { drawTextEntry(); return; }
 */

import { decodeDelta } from './input_filter.mjs';

/* Constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const MAX_BUFFER_LENGTH = 512;
const PREVIEW_DURATION_TICKS = 180;  /* ~3 seconds at 60fps */

/* Layout constants */
const TITLE_Y = 2;
const RULE_Y = 12;
const GRID_START_Y = 15;
const ROW_HEIGHT = 11;
const CHAR_WIDTH = 14;
const CHARS_PER_ROW = 8;
const GRID_START_X = 6;

/* MIDI CC values */
const CC_JOG_WHEEL = 14;
const CC_JOG_CLICK = 3;
const CC_BACK = 51;

/* Character pages */
const PAGES = [
    'abcdefghijklmnopqrstuvwxyz',
    'ABCDEFGHIJKLMNOPQRSTUVWXYZ',
    '1234567890.-!@#$%^&*',
    '\'";:?/\\<>()[]{}=-+'
];

/* Special button indices (relative to end of character set) */
const SPECIAL_PAGE = 0;
const SPECIAL_SPACE = 1;
const SPECIAL_BACKSPACE = 2;
const SPECIAL_CONFIRM = 3;
const NUM_SPECIALS = 4;

/* State */
let state = {
    active: false,
    title: '',
    buffer: '',
    selectedIndex: 0,
    page: 0,
    showingPreview: false,
    previewTimeout: 0,
    lastAction: null,      /* 'char', 'space', 'backspace' */
    lastChar: '',          /* Last character entered (for repeat) */
    onConfirm: null,
    onCancel: null
};

/**
 * Open the text entry keyboard
 * @param {Object} options
 * @param {string} [options.title=''] - Title to show at top
 * @param {string} [options.initialText=''] - Pre-populate buffer
 * @param {Function} options.onConfirm - Called with final text on confirm
 * @param {Function} [options.onCancel] - Called when user cancels
 */
export function openTextEntry({ title = '', initialText = '', onConfirm, onCancel }) {
    state.active = true;
    state.title = title;
    state.buffer = initialText.slice(0, MAX_BUFFER_LENGTH);
    state.selectedIndex = 0;
    state.page = 0;
    state.showingPreview = false;
    state.previewTimeout = 0;
    state.lastAction = null;
    state.lastChar = '';
    state.onConfirm = onConfirm;
    state.onCancel = onCancel || null;
}

/**
 * Close the text entry keyboard
 */
export function closeTextEntry() {
    state.active = false;
    state.onConfirm = null;
    state.onCancel = null;
}

/**
 * Check if text entry is currently active
 * @returns {boolean}
 */
export function isTextEntryActive() {
    return state.active;
}

/**
 * Get the current buffer contents
 * @returns {string}
 */
export function getTextEntryBuffer() {
    return state.buffer;
}

/**
 * Handle MIDI input for text entry
 * @param {Uint8Array|Array} msg - MIDI message
 * @returns {boolean} true if message was handled
 */
export function handleTextEntryMidi(msg) {
    if (!state.active) return false;

    const status = msg[0] & 0xF0;
    const data1 = msg[1];
    const data2 = msg[2];

    /* Only handle CC messages */
    if (status !== 0xB0) return false;

    const cc = data1;
    const value = data2;
    const isDown = value > 0;

    /* If showing preview, jog wheel turn dismisses, click repeats last action */
    if (state.showingPreview) {
        /* Jog wheel turn - dismiss and navigate */
        if (cc === CC_JOG_WHEEL) {
            state.showingPreview = false;
            state.previewTimeout = 0;
            /* Fall through to handle navigation */
        }
        /* Jog click down - repeat last action */
        else if (cc === CC_JOG_CLICK && isDown) {
            repeatLastAction();
            return true;
        }
        /* Ignore other inputs (including jog release) while preview showing */
        else {
            return true;
        }
    }

    /* Jog wheel - navigate */
    if (cc === CC_JOG_WHEEL) {
        const delta = decodeDelta(value);
        if (delta !== 0) {
            const totalItems = getCurrentPageChars().length + NUM_SPECIALS;
            let newIndex = state.selectedIndex + delta;
            /* Clamp to edges (no wrap) - allows slamming left/right to reach a or OK */
            if (newIndex < 0) newIndex = 0;
            if (newIndex >= totalItems) newIndex = totalItems - 1;
            state.selectedIndex = newIndex;
        }
        return true;
    }

    /* Jog click - select */
    if (cc === CC_JOG_CLICK && isDown) {
        handleSelection();
        return true;
    }

    /* Back button - cancel */
    if (cc === CC_BACK && isDown) {
        if (state.onCancel) {
            state.onCancel();
        }
        closeTextEntry();
        return true;
    }

    return false;
}

/**
 * Handle selection of current item
 */
function handleSelection() {
    const chars = getCurrentPageChars();
    const charCount = chars.length;

    if (state.selectedIndex < charCount) {
        /* Character selected */
        const char = chars[state.selectedIndex];
        appendToBuffer(char);
        state.lastAction = 'char';
        state.lastChar = char;
        showPreview();
    } else {
        /* Special button selected */
        const specialIndex = state.selectedIndex - charCount;
        switch (specialIndex) {
            case SPECIAL_PAGE:
                /* Cycle to next page */
                state.page = (state.page + 1) % PAGES.length;
                /* Keep page button selected on new page */
                const newChars = getCurrentPageChars();
                state.selectedIndex = newChars.length + SPECIAL_PAGE;
                break;
            case SPECIAL_SPACE:
                appendToBuffer(' ');
                state.lastAction = 'space';
                showPreview();
                break;
            case SPECIAL_BACKSPACE:
                if (state.buffer.length > 0) {
                    state.buffer = state.buffer.slice(0, -1);
                }
                state.lastAction = 'backspace';
                showPreview();
                break;
            case SPECIAL_CONFIRM:
                if (state.onConfirm) {
                    state.onConfirm(state.buffer);
                }
                closeTextEntry();
                break;
        }
    }
}

/**
 * Repeat the last action (for quick edits during preview)
 */
function repeatLastAction() {
    switch (state.lastAction) {
        case 'char':
            appendToBuffer(state.lastChar);
            break;
        case 'space':
            appendToBuffer(' ');
            break;
        case 'backspace':
            if (state.buffer.length > 0) {
                state.buffer = state.buffer.slice(0, -1);
            }
            break;
    }
    /* Reset preview timeout */
    state.previewTimeout = PREVIEW_DURATION_TICKS;
}

/**
 * Append character to buffer
 */
function appendToBuffer(char) {
    if (state.buffer.length < MAX_BUFFER_LENGTH) {
        state.buffer += char;
    }
}

/**
 * Show the preview screen
 */
function showPreview() {
    state.showingPreview = true;
    state.previewTimeout = PREVIEW_DURATION_TICKS;
}

/**
 * Get characters for current page
 */
function getCurrentPageChars() {
    return PAGES[state.page];
}

/**
 * Tick the text entry (call in your tick function)
 * @returns {boolean} true if state changed
 */
export function tickTextEntry() {
    if (!state.active) return false;

    if (state.showingPreview && state.previewTimeout > 0) {
        state.previewTimeout--;
        if (state.previewTimeout === 0) {
            state.showingPreview = false;
            return true;
        }
    }
    return false;
}

/**
 * Draw the text entry screen (call in your tick/draw function)
 */
export function drawTextEntry() {
    if (!state.active) return;

    clear_screen();

    if (state.showingPreview) {
        drawPreviewScreen();
    } else {
        drawKeyboardScreen();
    }
}

/**
 * Draw the keyboard grid screen
 */
function drawKeyboardScreen() {
    /* Title with current buffer */
    const bufferDisplay = state.buffer || 'Untitled';
    if (state.title) {
        /* Combine title and buffer, truncate if needed */
        const combined = `${state.title}: ${bufferDisplay}`;
        const maxChars = Math.floor((SCREEN_WIDTH - 4) / 6);  /* 6px per char */
        const displayText = combined.length > maxChars
            ? combined.slice(0, maxChars - 1) + '…'
            : combined;
        print(2, TITLE_Y, displayText, 1);
    } else {
        /* No title, just show buffer */
        print(2, TITLE_Y, bufferDisplay, 1);
    }
    fill_rect(0, RULE_Y, SCREEN_WIDTH, 1, 1);

    const chars = getCurrentPageChars();
    const charCount = chars.length;
    const totalItems = charCount + NUM_SPECIALS;

    /* Calculate grid dimensions */
    const numCharRows = Math.ceil(charCount / CHARS_PER_ROW);

    /* Draw character grid */
    for (let i = 0; i < charCount; i++) {
        const row = Math.floor(i / CHARS_PER_ROW);
        const col = i % CHARS_PER_ROW;
        const x = GRID_START_X + col * CHAR_WIDTH;
        const y = GRID_START_Y + row * ROW_HEIGHT;
        const isSelected = i === state.selectedIndex;

        if (isSelected) {
            fill_rect(x - 2, y - 1, CHAR_WIDTH - 2, ROW_HEIGHT, 1);
            print(x, y, chars[i], 0);
        } else {
            print(x, y, chars[i], 1);
        }
    }

    /* Draw special buttons on fixed bottom row */
    drawSpecialButtons(charCount);
}

/**
 * Draw special buttons (page switch, space, backspace, confirm)
 * Always drawn at fixed position on bottom row, right-aligned
 */
function drawSpecialButtons(charCount) {
    /* Fixed position: row 4 (bottom), right-aligned */
    const specialY = GRID_START_Y + 3 * ROW_HEIGHT;

    /* Button definitions: label, width */
    const buttons = [
        { label: '...', width: 18 },      /* Page switch */
        { label: '___', width: 24 },      /* Space */
        { label: 'x', width: 14 },        /* Backspace */
        { label: 'OK', width: 18 }        /* Confirm */
    ];

    /* Calculate total width for right-alignment */
    const totalWidth = buttons.reduce((sum, btn) => sum + btn.width + 2, 0) - 2;
    const buttonsStartX = SCREEN_WIDTH - totalWidth - 2;

    let x = buttonsStartX;
    for (let i = 0; i < buttons.length; i++) {
        const btn = buttons[i];
        const isSelected = state.selectedIndex === charCount + i;
        const btnHeight = ROW_HEIGHT + 1;

        if (isSelected) {
            fill_rect(x, specialY + 2, btn.width, btnHeight, 1);
            print(x + 2, specialY + 1, btn.label, 0);
        } else {
            /* Draw button outline */
            draw_rect(x, specialY + 2, btn.width, btnHeight, 1);
            print(x + 2, specialY + 1, btn.label, 1);
        }

        x += btn.width + 2;
    }
}

/**
 * Draw outlined rectangle
 */
function draw_rect(x, y, w, h, color) {
    fill_rect(x, y, w, 1, color);           /* Top */
    fill_rect(x, y + h - 1, w, 1, color);   /* Bottom */
    fill_rect(x, y, 1, h, color);           /* Left */
    fill_rect(x + w - 1, y, 1, h, color);   /* Right */
}

/**
 * Draw the preview screen showing current buffer
 */
function drawPreviewScreen() {
    /* Center the text vertically */
    const centerY = (SCREEN_HEIGHT - 10) / 2;

    /* Format buffer with cursor */
    let displayText = state.buffer + '_';

    /* Truncate if too long */
    const maxDisplayChars = 20;
    if (displayText.length > maxDisplayChars) {
        displayText = '...' + displayText.slice(-(maxDisplayChars - 3));
    }

    /* Center horizontally */
    const textWidth = displayText.length * 6;
    const centerX = (SCREEN_WIDTH - textWidth) / 2;

    print(Math.max(2, centerX), centerY, displayText, 1);
}
