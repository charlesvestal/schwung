/*
 * Text Entry Test Module
 *
 * Simple test harness for the text_entry.mjs component.
 * Press jog click to open keyboard, type text, confirm or cancel.
 */

import {
    openTextEntry,
    isTextEntryActive,
    handleTextEntryMidi,
    drawTextEntry,
    tickTextEntry
} from '../../shared/text_entry.mjs';

/* State */
let lastEnteredText = "(none)";
let lastAction = "Ready";

/* MIDI CCs */
const CC_JOG_CLICK = 3;
const CC_BACK = 51;

/* Screen dimensions */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

function drawMainScreen() {
    clear_screen();

    /* Title */
    print(2, 2, "Text Entry Test", 1);
    fill_rect(0, 12, SCREEN_WIDTH, 1, 1);

    /* Instructions */
    print(2, 18, "Click jog: New text", 1);
    print(2, 29, "Shift+click: Edit", 1);
    print(2, 40, "Back: Return to menu", 1);

    /* Current state */
    fill_rect(0, 52, SCREEN_WIDTH, 1, 1);

    /* Truncate text for display */
    let displayText = lastEnteredText;
    if (displayText.length > 18) {
        displayText = displayText.slice(0, 15) + "...";
    }
    print(2, 54, `${lastAction}: ${displayText}`, 1);
}

globalThis.init = function() {
    lastEnteredText = "(none)";
    lastAction = "Ready";
};

globalThis.tick = function() {
    /* Text entry takes over when active */
    if (isTextEntryActive()) {
        tickTextEntry();
        drawTextEntry();
        return;
    }

    drawMainScreen();
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    const cc = data[1];
    const val = data[2];

    /* Text entry handles its own MIDI when active */
    if (isTextEntryActive()) {
        handleTextEntryMidi(data);
        return;
    }

    /* Only handle CC messages */
    if (status !== 0xB0) return;

    const isDown = val > 0;

    /* Jog click - open text entry */
    if (cc === CC_JOG_CLICK && isDown) {
        /* Check if shift is held (would need to track, but for demo just use empty) */
        openTextEntry({
            title: "Enter Text",
            initialText: lastEnteredText === "(none)" ? "" : lastEnteredText,
            onConfirm: (text) => {
                lastEnteredText = text || "(empty)";
                lastAction = "Saved";
            },
            onCancel: () => {
                lastAction = "Cancelled";
            }
        });
        return;
    }

    /* Back - return to menu */
    if (cc === CC_BACK && isDown) {
        host_return_to_menu();
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {
    /* Pass external MIDI through if text entry is active */
    if (isTextEntryActive()) {
        handleTextEntryMidi(data);
    }
};
