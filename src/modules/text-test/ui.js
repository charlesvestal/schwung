/*
 * Text Entry Test Module
 *
 * Test harness for the text_entry.mjs component with live knob tuning.
 * Press jog click to open keyboard with pad selection enabled.
 * Knobs 1-4 adjust velocity threshold, aftertouch threshold,
 * aftertouch rearm, and slide guard ms in real time.
 * Diagnostics shown in the keyboard title bar while active.
 */

import {
    openTextEntry,
    isTextEntryActive,
    handleTextEntryMidi,
    drawTextEntry,
    tickTextEntry,
    setTextEntryTitle,
    padConfig
} from '/data/UserData/move-anything/shared/text_entry.mjs';

/* State */
let lastEnteredText = "(none)";
let lastAction = "Ready";

/* MIDI CCs */
const CC_JOG_CLICK = 3;
const CC_BACK = 51;
const CC_KNOB1 = 71;
const CC_KNOB2 = 72;
const CC_KNOB3 = 73;
const CC_KNOB4 = 74;

/* Screen dimensions */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* Knob ranges */
const KNOB_RANGES = {
    velocityThreshold:   { min: 1, max: 127, step: 1 },
    aftertouchThreshold: { min: 1, max: 127, step: 1 },
    aftertouchRearm:     { min: 1, max: 40,  step: 1 },
    slideGuardMs:        { min: 0, max: 500, step: 10 }
};

/* Map CC to padConfig key */
const KNOB_MAP = {
    [CC_KNOB1]: 'velocityThreshold',
    [CC_KNOB2]: 'aftertouchThreshold',
    [CC_KNOB3]: 'aftertouchRearm',
    [CC_KNOB4]: 'slideGuardMs'
};

function applyKnobDelta(key, delta) {
    const range = KNOB_RANGES[key];
    let val = padConfig[key] + delta * range.step;
    if (val < range.min) val = range.min;
    if (val > range.max) val = range.max;
    padConfig[key] = val;
}

function diagTitle() {
    return `V${padConfig.velocityThreshold} A${padConfig.aftertouchThreshold} R${padConfig.aftertouchRearm} S${padConfig.slideGuardMs}`;
}

function drawMainScreen() {
    clear_screen();

    print(2, 2, "Text Entry Test", 1);
    fill_rect(0, 12, SCREEN_WIDTH, 1, 1);

    print(2, 15, `K1 Vel:${padConfig.velocityThreshold}`, 1);
    print(66, 15, `K2 AT:${padConfig.aftertouchThreshold}`, 1);
    print(2, 26, `K3 Rearm:${padConfig.aftertouchRearm}`, 1);
    print(66, 26, `K4 Slide:${padConfig.slideGuardMs}`, 1);

    print(2, 39, "Click jog: Open kbd", 1);

    fill_rect(0, 52, SCREEN_WIDTH, 1, 1);
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
    if (isTextEntryActive()) {
        setTextEntryTitle(diagTitle());
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

    /* Knob turns always adjust padConfig, even while keyboard is open */
    if (status === 0xB0) {
        const key = KNOB_MAP[cc];
        if (key) {
            let delta = val <= 63 ? val : val - 128;
            applyKnobDelta(key, delta);
            return;
        }
    }

    /* Text entry handles its own MIDI when active */
    if (isTextEntryActive()) {
        handleTextEntryMidi(data);
        return;
    }

    /* CC messages */
    if (status === 0xB0) {
        const isDown = val > 0;

        /* Jog click - open text entry with pad selection */
        if (cc === CC_JOG_CLICK && isDown) {
            openTextEntry({
                title: diagTitle(),
                initialText: lastEnteredText === "(none)" ? "" : lastEnteredText,
                padSelect: true,
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
    }
};

globalThis.onMidiMessageExternal = function(data) {
    if (isTextEntryActive()) {
        handleTextEntryMidi(data);
    }
};
