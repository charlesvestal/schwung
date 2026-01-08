/*
 * Signal Chain Module UI
 *
 * Phase 1: Basic status display showing loaded synth
 * Future: Patch browser, parameter editing
 */

import { isCapacitiveTouchMessage } from '../../shared/input_filter.mjs';

/* State */
let synthName = "SF2 Synth";
let presetName = "";
let polyphony = 0;

let needsRedraw = true;
let tickCount = 0;
const REDRAW_INTERVAL = 6;

/* Display constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* Draw the UI */
function drawUI() {
    clear_screen();

    /* Title bar */
    print(2, 2, "Signal Chain", 1);
    fill_rect(0, 14, SCREEN_WIDTH, 1, 1);

    /* Chain status */
    print(2, 20, "Synth:", 1);
    print(40, 20, synthName, 1);

    /* Preset info from synth */
    const pn = host_module_get_param("preset_name");
    if (pn) {
        presetName = pn.substring(0, 16);
    }
    print(2, 32, presetName, 1);

    /* Polyphony */
    print(2, 44, `Voices: ${polyphony}`, 1);

    /* Phase indicator */
    print(2, 54, "Phase 1: Basic", 1);

    needsRedraw = false;
}

/* === Required module UI callbacks === */

globalThis.init = function() {
    console.log("Signal Chain UI initializing...");
    needsRedraw = true;
    console.log("Signal Chain UI ready");
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

    /* Note activity triggers redraw for visual feedback */
    if (status === 0x90 || status === 0x80) {
        needsRedraw = true;
    }
};

globalThis.onMidiMessageExternal = function(data) {
    /* External MIDI goes to DSP via host */
};
