/*
 * DX7 Synth Module UI
 *
 * Provides UI for the DX7 FM synthesizer module.
 * Handles preset selection, octave transpose, and display updates.
 */

import {
    MoveMainKnob,
    MoveLeft, MoveRight, MoveUp, MoveDown,
    MovePads
} from '../../shared/constants.mjs';

import { isCapacitiveTouchMessage } from '../../shared/input_filter.mjs';

/* State */
let currentPreset = 0;
let presetCount = 32;  /* Standard DX7 bank has 32 presets */
let patchName = "Init";
let algorithm = 1;
let octaveTranspose = 0;
let polyphony = 0;

/* Alias constants for clarity */
const CC_JOG_WHEEL = MoveMainKnob;
const CC_LEFT = MoveLeft;
const CC_RIGHT = MoveRight;
const CC_PLUS = MoveUp;
const CC_MINUS = MoveDown;

/* Note range for Move pads */
const PAD_NOTE_MIN = MovePads[0];
const PAD_NOTE_MAX = MovePads[MovePads.length - 1];

let needsRedraw = true;
let tickCount = 0;
const REDRAW_INTERVAL = 6;  /* Redraw every 6 ticks (~10Hz) */

/* Display constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* DX7 algorithm display - simplified shapes */
const ALG_DISPLAY = [
    "1->2->3->4->5->6",  /* Alg 1 */
    "1->2  3->4->5->6",
    "1->2->3  4->5->6",
    "1->2->3  4->5  6",
    "1->2 3->4 5->6",
    "1->2->3->4->5  6",
    /* ... simplified for display */
];

/* Draw the UI */
function drawUI() {
    clear_screen();

    /* Title bar */
    print(2, 2, "DX7 Synth", 1);
    fill_rect(0, 12, SCREEN_WIDTH, 1, 1);

    /* Preset number and name */
    const presetStr = String(currentPreset + 1).padStart(2, '0');
    print(2, 18, `${presetStr}: ${patchName}`, 1);

    /* Algorithm display */
    print(2, 30, `Algorithm: ${algorithm}`, 1);

    /* Octave and polyphony */
    const octStr = octaveTranspose >= 0 ? `+${octaveTranspose}` : `${octaveTranspose}`;
    print(2, 42, `Oct:${octStr}  Voices:${polyphony}`, 1);

    needsRedraw = false;
}

/* Send all notes off to DSP (triggers release phase) */
function allNotesOff() {
    host_module_set_param("all_notes_off", "1");
}

/* Panic - immediately silence all voices (for preset changes) */
function panic() {
    host_module_set_param("panic", "1");
}

/* Change preset */
function setPreset(index) {
    if (index < 0) index = presetCount - 1;
    if (index >= presetCount) index = 0;

    panic();  /* Full reset on preset change */
    currentPreset = index;
    host_module_set_param("preset", String(currentPreset));

    /* Get patch info from DSP */
    const name = host_module_get_param("patch_name");
    if (name) {
        patchName = name.trim();
    } else {
        patchName = `Patch ${currentPreset + 1}`;
    }

    const alg = host_module_get_param("algorithm");
    if (alg) {
        algorithm = parseInt(alg) || 1;
    }

    needsRedraw = true;
    console.log(`DX7: Preset ${currentPreset + 1}: ${patchName} (Alg ${algorithm})`);
}

/* Change octave */
function setOctave(delta) {
    allNotesOff();
    octaveTranspose += delta;
    if (octaveTranspose < -4) octaveTranspose = -4;
    if (octaveTranspose > 4) octaveTranspose = 4;

    host_module_set_param("octave_transpose", String(octaveTranspose));

    needsRedraw = true;
    console.log(`DX7: Octave transpose: ${octaveTranspose}`);
}

/* Handle CC messages */
function handleCC(cc, value) {
    /* Note: Shift+Wheel exit is handled at host level */

    /* Preset navigation */
    if (cc === CC_LEFT && value > 0) {
        setPreset(currentPreset - 1);
        return true;
    }
    if (cc === CC_RIGHT && value > 0) {
        setPreset(currentPreset + 1);
        return true;
    }

    /* Octave */
    if (cc === CC_PLUS && value > 0) {
        setOctave(1);
        return true;
    }
    if (cc === CC_MINUS && value > 0) {
        setOctave(-1);
        return true;
    }

    /* Jog wheel */
    if (cc === CC_JOG_WHEEL) {
        if (value === 1) {
            setPreset(currentPreset + 1);
        } else if (value === 127 || value === 65) {
            setPreset(currentPreset - 1);
        }
        return true;
    }

    return false;
}

/* === Required module UI callbacks === */

globalThis.init = function() {
    console.log("DX7 UI initializing...");

    /* Get initial state from DSP */
    const pc = host_module_get_param("preset_count");
    if (pc) presetCount = parseInt(pc) || 32;

    const pn = host_module_get_param("patch_name");
    if (pn) patchName = pn.trim();

    const cp = host_module_get_param("preset");
    if (cp) currentPreset = parseInt(cp) || 0;

    const alg = host_module_get_param("algorithm");
    if (alg) algorithm = parseInt(alg) || 1;

    needsRedraw = true;
    console.log(`DX7 UI ready: ${presetCount} presets`);
};

globalThis.tick = function() {
    /* Update polyphony from DSP */
    const poly = host_module_get_param("polyphony");
    if (poly) {
        polyphony = parseInt(poly) || 0;
    }

    /* Rate-limited redraw */
    tickCount++;
    if (needsRedraw || tickCount >= REDRAW_INTERVAL) {
        drawUI();
        tickCount = 0;
        needsRedraw = false;
    }
};

globalThis.onMidiMessageInternal = function(data) {
    if (isCapacitiveTouchMessage(data)) return;

    const status = data[0] & 0xF0;
    const isNote = status === 0x90 || status === 0x80;

    if (status === 0xB0) {
        if (handleCC(data[1], data[2])) {
            return;
        }
    } else if (isNote) {
        needsRedraw = true;
    }
};

globalThis.onMidiMessageExternal = function(data) {
    /* External MIDI goes to DSP via host */
};
