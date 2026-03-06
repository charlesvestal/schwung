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
import { drawRect } from './menu_layout.mjs';

/* Pad LED constants */
const PAD_NOTE_START = 68;
const PAD_NOTE_END = 99;
const PAD_COLOR_WHITE = 120;
const PAD_COLOR_HIGHLIGHT = 8;   /* BrightGreen */
const PAD_COLOR_OFF = 0;
const PAD_COLOR_PURPLE = 48;
const PAD_COLOR_BLUE = 44;
const PAD_COLOR_RED = 4;
const PAD_COLOR_GREEN = 16;
const MIDI_NOTE_ON = 0x90;
/* Global pad select setting — toggled via Settings > Display > Pad Typing */
export let padSelectGlobal = false;
export function setPadSelectGlobal(enabled) { padSelectGlobal = !!enabled; }

/* Tunable pad entry parameters */
export let padConfig = {
    velocityThreshold: 31,
    aftertouchThreshold: 2,
    aftertouchRearm: 1,
    slideGuardMs: 200
};

/* LED state for pad select mode */
let currentHighlightPad = -1;    /* Currently highlighted pad note */
let pressureFired = false;       /* True after aftertouch triggered entry, rearms below REARM */
let padLedSnapshot = {};         /* note -> original color, for restore */
let lastPadNote = -1;            /* Note of last pad touch */
let pendingEntryNote = -1;       /* Pad note awaiting deferred velocity entry */
let pendingEntryTime = 0;        /* Date.now() when deferred entry was queued */

function sendPadLED(note, color) {
    if (typeof move_midi_internal_send === 'function') {
        move_midi_internal_send([0x09, MIDI_NOTE_ON, note, color]);
    }
}

function snapshotPadLEDs() {
    padLedSnapshot = {};
    if (typeof shadow_get_pad_led_snapshot === 'function') {
        const snap = shadow_get_pad_led_snapshot();
        if (snap) {
            for (let note = PAD_NOTE_START; note <= PAD_NOTE_END; note++) {
                const color = snap[String(note)];
                if (color !== undefined) padLedSnapshot[note] = color;
            }
        }
    }
}

/* Special keys always occupy cols 3-7 of the last character row.
 * Layout: [chars...] [blank] [page/purple] [space/blue] [space/blue] [del/red] [ok/green] */
const SPECIAL_COL_START = 3;  /* First special at col 3 (after gap at col 2) */

/* Get the default LED color for a pad in the current grid layout */
function getPadDefaultColor(padNote) {
    const padIndex = padNote - PAD_NOTE_START;
    const padCol = padIndex % 8;
    const padRow = 3 - Math.floor(padIndex / 8);
    const chars = getCurrentPageChars();
    const charCount = chars.length;
    const numCharRows = Math.ceil(charCount / CHARS_PER_ROW);
    const gridIndex = padRow * CHARS_PER_ROW + padCol;
    const lastCharRow = numCharRows - 1;

    /* Character rows above the last row */
    if (padRow < lastCharRow && gridIndex < charCount) {
        return PAD_COLOR_WHITE;
    }
    /* Last character row: chars in cols before remainder, then gap, then specials */
    if (padRow === lastCharRow) {
        if (gridIndex < charCount) return PAD_COLOR_WHITE;  /* Remaining chars */
        if (padCol >= SPECIAL_COL_START) {
            const sk = padCol - SPECIAL_COL_START;
            if (sk === 0) return PAD_COLOR_PURPLE;  /* page cycle */
            if (sk === 1 || sk === 2) return PAD_COLOR_BLUE;  /* space */
            if (sk === 3) return PAD_COLOR_RED;     /* delete */
            if (sk === 4) return PAD_COLOR_GREEN;   /* ok */
        }
    }
    return PAD_COLOR_OFF;
}

/* Map a pad column to a special index, or -1 if not a special */
function padColToSpecial(padCol) {
    const sk = padCol - SPECIAL_COL_START;
    if (sk === 0) return SPECIAL_PAGE;
    if (sk === 1 || sk === 2) return SPECIAL_SPACE;
    if (sk === 3) return SPECIAL_BACKSPACE;
    if (sk === 4) return SPECIAL_CONFIRM;
    return -1;
}

function setupPadLEDs() {
    currentHighlightPad = -1;
    for (let padNote = PAD_NOTE_START; padNote <= PAD_NOTE_END; padNote++) {
        sendPadLED(padNote, getPadDefaultColor(padNote));
    }
}

function restorePadLEDs() {
    /* Restore snapshot colors */
    for (let padNote = PAD_NOTE_START; padNote <= PAD_NOTE_END; padNote++) {
        const color = padLedSnapshot[padNote] || 0;
        sendPadLED(padNote, color);
    }
    currentHighlightPad = -1;
}

/* Map a pad note to the correct selectedIndex */
function selectPadItem(padNote) {
    const padIndex = padNote - PAD_NOTE_START;
    const padCol = padIndex % 8;
    const padRow = 3 - Math.floor(padIndex / 8);
    const chars = getCurrentPageChars();
    const charCount = chars.length;
    const numCharRows = Math.ceil(charCount / CHARS_PER_ROW);
    const gridIndex = padRow * CHARS_PER_ROW + padCol;
    const lastCharRow = numCharRows - 1;

    if (padRow < lastCharRow && gridIndex < charCount) {
        state.selectedIndex = gridIndex;
    } else if (padRow === lastCharRow) {
        if (gridIndex < charCount) {
            state.selectedIndex = gridIndex;
        } else if (padCol >= SPECIAL_COL_START) {
            const sp = padColToSpecial(padCol);
            if (sp >= 0) state.selectedIndex = charCount + sp;
        }
    }
}

function highlightPad(padNote) {
    if (currentHighlightPad === padNote) return;
    /* Restore previous highlight to its default color */
    if (currentHighlightPad >= PAD_NOTE_START) {
        sendPadLED(currentHighlightPad, getPadDefaultColor(currentHighlightPad));
    }
    /* Highlight new pad */
    if (padNote >= PAD_NOTE_START && padNote <= PAD_NOTE_END) {
        sendPadLED(padNote, PAD_COLOR_HIGHLIGHT);
    }
    currentHighlightPad = padNote;
}

/* Constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const MAX_BUFFER_LENGTH = 512;
const PREVIEW_DURATION_TICKS = 180;      /* ~3 seconds at 60fps */
const PREVIEW_DURATION_PAD_TICKS = 60;   /* ~1 second for pad entry */

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
    padSelect: false,      /* Enable pad-based character selection */
    onConfirm: null,
    onCancel: null,
    onAnnounce: null
};

/**
 * Open the text entry keyboard
 * @param {Object} options
 * @param {string} [options.title=''] - Title to show at top
 * @param {string} [options.initialText=''] - Pre-populate buffer
 * @param {Function} options.onConfirm - Called with final text on confirm
 * @param {Function} [options.onCancel] - Called when user cancels
 * @param {Function} [options.onAnnounce] - Screen reader announce callback
 * @param {boolean} [options.padSelect=false] - Enable pad-based character selection
 */
export function openTextEntry({ title = '', initialText = '', onConfirm, onCancel, onAnnounce = null, padSelect } = {}) {
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
    /* Use caller's explicit padSelect if provided, otherwise fall back to global setting */
    state.padSelect = (padSelect !== undefined) ? !!padSelect : padSelectGlobal;
    state.onCancel = onCancel || null;
    state.onAnnounce = (typeof onAnnounce === 'function') ? onAnnounce : null;

    if (state.padSelect) {
        snapshotPadLEDs();
        if (typeof host_pad_block === 'function') host_pad_block(1);
        setupPadLEDs();
    }

    announceTextEntry(`Text entry, ${state.title || "Edit text"}. Current text: ${getAnnounceBuffer()}. ${getSelectedLabel()} selected`);
}

/**
 * Close the text entry keyboard
 */
export function closeTextEntry() {
    if (state.padSelect) {
        restorePadLEDs();
        if (typeof host_pad_block === 'function') host_pad_block(0);
    }
    state.active = false;
    state.onConfirm = null;
    state.onCancel = null;
    state.onAnnounce = null;
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
 * Update the title while text entry is active
 * @param {string} newTitle
 */
export function setTextEntryTitle(newTitle) {
    state.title = newTitle;
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

    /* Pad press (Note On) — select, or enter if pressed hard enough */
    if (state.padSelect && status === 0x90 && data1 >= 68 && data1 <= 99) {
        if (data2 === 0) return true;  /* Block note-off */
        if (state.showingPreview) {
            state.showingPreview = false;
            state.previewTimeout = 0;
        }
        /* Cancel any pending deferred entry from previous pad (slide detected) */
        pendingEntryNote = -1;
        lastPadNote = data1;
        pressureFired = false;  /* Rearm aftertouch on new pad touch */
        selectPadItem(data1);
        highlightPad(data1);
        if (data2 >= padConfig.velocityThreshold) {
            /* Defer entry — tick will commit if no new pad arrives within window */
            pendingEntryNote = data1;
            pendingEntryTime = Date.now();
        } else {
            announceTextEntry(`${getSelectedLabel()} selected`);
        }
        return true;
    }

    /* Polyphonic aftertouch on pad — also enters if pressed harder after touch */
    if (state.padSelect && status === 0xA0 && data1 >= 68 && data1 <= 99) {
        if (data2 >= padConfig.aftertouchThreshold && !pressureFired) {
            pressureFired = true;
            selectPadItem(data1);
            handleSelection(true);
        } else if (data2 < padConfig.aftertouchRearm) {
            pressureFired = false;
        }
        return true;
    }

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
            if (newIndex !== state.selectedIndex) {
                state.selectedIndex = newIndex;
                announceTextEntry(`${getSelectedLabel()} selected`);
            }
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
        announceTextEntry("Text entry cancelled");
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
function handleSelection(fromPad) {
    const chars = getCurrentPageChars();
    const charCount = chars.length;

    if (state.selectedIndex < charCount) {
        /* Character selected */
        const char = chars[state.selectedIndex];
        appendToBuffer(char);
        state.lastAction = 'char';
        state.lastChar = char;
        announceTextEntry(`${char} entered, text ${getAnnounceBuffer()}`);
        showPreview(fromPad);
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
                if (state.padSelect) setupPadLEDs();
                announceTextEntry(`Keyboard page ${state.page + 1}, ${getSelectedLabel()} selected`);
                break;
            case SPECIAL_SPACE:
                appendToBuffer(' ');
                state.lastAction = 'space';
                announceTextEntry(`space entered, text ${getAnnounceBuffer()}`);
                showPreview(fromPad);
                break;
            case SPECIAL_BACKSPACE:
                if (state.buffer.length > 0) {
                    state.buffer = state.buffer.slice(0, -1);
                }
                state.lastAction = 'backspace';
                announceTextEntry(`Deleted, text ${getAnnounceBuffer()}`);
                showPreview(fromPad);
                break;
            case SPECIAL_CONFIRM:
                announceTextEntry(`Text entry confirmed, text ${getAnnounceBuffer()}`);
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
            announceTextEntry(`${state.lastChar} entered, text ${getAnnounceBuffer()}`);
            break;
        case 'space':
            appendToBuffer(' ');
            announceTextEntry(`space entered, text ${getAnnounceBuffer()}`);
            break;
        case 'backspace':
            if (state.buffer.length > 0) {
                state.buffer = state.buffer.slice(0, -1);
            }
            announceTextEntry(`Deleted, text ${getAnnounceBuffer()}`);
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
function showPreview(fromPad) {
    state.showingPreview = true;
    state.previewTimeout = fromPad ? PREVIEW_DURATION_PAD_TICKS : PREVIEW_DURATION_TICKS;
}

/**
 * Get characters for current page
 */
function getCurrentPageChars() {
    return PAGES[state.page];
}

function announceTextEntry(text) {
    if (!state.onAnnounce || !text) return;
    try {
        state.onAnnounce(text);
    } catch (e) {
        /* Never let screen reader callback errors break UI input flow. */
    }
}

function getSelectedLabel() {
    const chars = getCurrentPageChars();
    if (state.selectedIndex < chars.length) return chars[state.selectedIndex];

    const specialIndex = state.selectedIndex - chars.length;
    switch (specialIndex) {
        case SPECIAL_PAGE: return "page";
        case SPECIAL_SPACE: return "space";
        case SPECIAL_BACKSPACE: return "delete";
        case SPECIAL_CONFIRM: return "OK";
        default: return "item";
    }
}

function getAnnounceBuffer() {
    if (!state.buffer || state.buffer.length === 0) return "Untitled";
    if (state.buffer.length > 24) return state.buffer.slice(-24);
    return state.buffer;
}

/**
 * Tick the text entry (call in your tick function)
 * @returns {boolean} true if state changed
 */
export function tickTextEntry() {
    if (!state.active) return false;

    /* Commit deferred velocity entry after slide guard window */
    if (pendingEntryNote >= 0 && (Date.now() - pendingEntryTime) >= padConfig.slideGuardMs) {
        const note = pendingEntryNote;
        pendingEntryNote = -1;
        if (lastPadNote === note) {
            selectPadItem(note);
            handleSelection(true);
        }
    }

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
            drawRect(x, specialY + 2, btn.width, btnHeight, 1);
            print(x + 2, specialY + 1, btn.label, 1);
        }

        x += btn.width + 2;
    }
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
