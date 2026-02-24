/*
 * Freeverb Chain UI
 *
 * Simple parameter editor for Freeverb when editing in chain mode.
 * Uses globalThis.chain_ui pattern for loading inside shadow UI.
 */

import {
    MoveMainKnob,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4
} from '/data/UserData/move-anything/shared/constants.mjs';

import { decodeDelta } from '/data/UserData/move-anything/shared/input_filter.mjs';

import {
    drawMenuHeader as drawHeader,
    drawMenuFooter as drawFooter
} from '/data/UserData/move-anything/shared/menu_layout.mjs';

/* Display constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* Parameters - matches module.json chain_params */
const PARAMS = [
    { key: "room_size", name: "Room Size", min: 0, max: 1, step: 0.05 },
    { key: "damping", name: "Damping", min: 0, max: 1, step: 0.05 },
    { key: "wet", name: "Wet", min: 0, max: 1, step: 0.05 },
    { key: "dry", name: "Dry", min: 0, max: 1, step: 0.05 }
];

/* State */
let selectedParam = 0;
let paramValues = [0.7, 0.5, 0.35, 0.65];  /* Defaults */
let needsRedraw = true;

/* Fetch current parameter values from DSP */
function fetchParams() {
    for (let i = 0; i < PARAMS.length; i++) {
        const val = host_module_get_param(PARAMS[i].key);
        if (val !== null && val !== undefined) {
            paramValues[i] = parseFloat(val) || PARAMS[i].min;
        }
    }
}

/* Set a parameter value */
function setParam(index, value) {
    const param = PARAMS[index];
    value = Math.max(param.min, Math.min(param.max, value));
    paramValues[index] = value;
    host_module_set_param(param.key, value.toFixed(2));
}

/* Adjust a parameter by delta */
function adjustParam(index, delta) {
    const param = PARAMS[index];
    const newVal = paramValues[index] + delta * param.step;
    setParam(index, newVal);
}

/* Draw the UI */
function drawUI() {
    clear_screen();
    drawHeader("Freeverb");

    const listY = 16;
    const lineHeight = 11;

    for (let i = 0; i < PARAMS.length; i++) {
        const y = listY + i * lineHeight;
        const param = PARAMS[i];
        const isSelected = i === selectedParam;

        if (isSelected) {
            fill_rect(0, y - 1, SCREEN_WIDTH, lineHeight, 1);
        }

        const color = isSelected ? 0 : 1;
        const prefix = isSelected ? "> " : "  ";

        /* Show parameter name */
        print(2, y, `${prefix}${param.name}`, color);

        /* Show value as percentage */
        const pct = Math.round(paramValues[i] * 100);
        const valueStr = `${pct}%`;
        print(SCREEN_WIDTH - valueStr.length * 6 - 4, y, valueStr, color);
    }

    drawFooter({left: "Jog: select", right: "Knobs: adjust"});
    needsRedraw = false;
}

/* Initialize */
function init() {
    fetchParams();
    needsRedraw = true;
}

/* Tick - called every frame */
function tick() {
    if (needsRedraw) {
        drawUI();
    }
}

/* Handle MIDI input */
function onMidiMessageInternal(data) {
    const status = data[0];
    const d1 = data[1];
    const d2 = data[2];

    /* Handle CC messages */
    if ((status & 0xF0) === 0xB0) {
        /* Jog wheel - select parameter */
        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta !== 0) {
                selectedParam = Math.max(0, Math.min(PARAMS.length - 1, selectedParam + delta));
                needsRedraw = true;
            }
            return;
        }

        /* Knobs 1-4 adjust corresponding parameters */
        if (d1 >= MoveKnob1 && d1 <= MoveKnob4) {
            const knobIndex = d1 - MoveKnob1;
            const delta = decodeDelta(d2);
            if (delta !== 0 && knobIndex < PARAMS.length) {
                adjustParam(knobIndex, delta);
                needsRedraw = true;
            }
            return;
        }
    }
}

/* Export as chain_ui for loading by shadow UI */
globalThis.chain_ui = {
    init,
    tick,
    onMidiMessageInternal
};
