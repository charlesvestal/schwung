/*
 * Signal Chain Module UI
 *
 * Phase 2: Patch browser with synth display
 */

import { isCapacitiveTouchMessage } from '../../shared/input_filter.mjs';
import { midiFxRegistry } from './midi_fx/index.mjs';

/* State */
let patchName = "";
let patchCount = 0;
let currentPatch = 0;
let synthModule = "";
let presetName = "";
let polyphony = 0;
let octaveTranspose = -2;
let octaveInitialized = false;
let midiFxJsSpec = "";
let midiFxJsChain = [];
let rawMidi = false;
let midiInput = "both";

let needsRedraw = true;
let tickCount = 0;
const REDRAW_INTERVAL = 6;

/* Display constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* MIDI CCs */
const CC_JOG = 14;
const CC_UP = 55;
const CC_DOWN = 54;

function handleCC(cc, val) {
    /* Jog wheel for patch navigation */
    if (cc === CC_JOG) {
        const delta = val < 64 ? val : val - 128;
        if (delta > 0) {
            nextPatch();
        } else if (delta < 0) {
            prevPatch();
        }
        return true;
    }

    /* Up/Down for octave (on button press) */
    if (cc === CC_UP && val === 127) {
        octaveUp();
        return true;
    }
    if (cc === CC_DOWN && val === 127) {
        octaveDown();
        return true;
    }

    return false;
}

function loadMidiFxChain(spec) {
    midiFxJsSpec = spec || "";
    midiFxJsChain = [];

    const names = midiFxJsSpec
        .split(',')
        .map((name) => name.trim())
        .filter((name) => name.length > 0);

    for (const name of names) {
        const fx = midiFxRegistry[name];
        if (!fx || typeof fx.processMidi !== "function") {
            console.log(`Chain UI: unknown MIDI FX '${name}'`);
            continue;
        }
        midiFxJsChain.push(fx);
    }
}

function processMidiFx(msg, source) {
    if (midiFxJsChain.length === 0) return;

    let messages = [msg];
    for (const fx of midiFxJsChain) {
        const next = [];
        for (const current of messages) {
            const out = fx.processMidi(current, source);
            if (Array.isArray(out)) {
                for (const o of out) {
                    if (o && o.length >= 3) next.push(o);
                }
            } else if (out && out.length >= 3) {
                next.push(out);
            }
        }
        messages = next;
        if (messages.length === 0) break;
    }

    for (const out of messages) {
        host_module_send_midi(out, "host");
    }
}

function midiSourceAllowed(source) {
    if (midiInput === "pads") {
        return source === "internal";
    }
    if (midiInput === "external") {
        return source === "external";
    }
    return true;
}

/* Draw the UI */
function drawUI() {
    clear_screen();

    /* Title bar with patch name */
    const title = patchName || "Signal Chain";
    print(2, 2, title, 1);
    fill_rect(0, 14, SCREEN_WIDTH, 1, 1);

    /* Patch navigation indicator */
    if (patchCount > 1) {
        const navText = `< ${currentPatch + 1}/${patchCount} >`;
        const navWidth = navText.length * 6;
        print(SCREEN_WIDTH - navWidth - 2, 2, navText, 1);
    }

    /* Synth module info */
    print(2, 20, "Synth:", 1);
    print(40, 20, synthModule || "SF2", 1);

    /* Preset from synth */
    const pn = host_module_get_param("preset_name");
    if (pn) {
        presetName = pn.substring(0, 16);
    }
    print(2, 32, presetName, 1);

    /* Octave and polyphony */
    const octStr = octaveTranspose >= 0 ? `+${octaveTranspose}` : `${octaveTranspose}`;
    print(2, 44, `Oct:${octStr}  Voices:${polyphony}`, 1);

    /* Hint for navigation */
    print(2, 54, "Jog:patch Up/Dn:oct", 1);

    needsRedraw = false;
}

/* Update state from DSP */
function updateState() {
    const pc = host_module_get_param("patch_count");
    if (pc) patchCount = parseInt(pc) || 0;

    const cp = host_module_get_param("current_patch");
    if (cp) currentPatch = parseInt(cp) || 0;

    const pn = host_module_get_param("patch_name");
    if (pn) patchName = pn;

    const sm = host_module_get_param("synth_module");
    if (sm) synthModule = sm;

    const poly = host_module_get_param("polyphony");
    if (poly) polyphony = parseInt(poly) || 0;

    const oct = host_module_get_param("octave_transpose");
    if (oct !== null && oct !== undefined) {
        octaveTranspose = parseInt(oct) || 0;
    }

    const rm = host_module_get_param("raw_midi");
    if (rm !== null && rm !== undefined) {
        rawMidi = parseInt(rm) === 1;
    }

    const mi = host_module_get_param("midi_input");
    if (mi) midiInput = mi;

    const fxSpec = host_module_get_param("midi_fx_js") || "";
    if (fxSpec !== midiFxJsSpec) {
        loadMidiFxChain(fxSpec);
    }
}

/* Switch to next/previous patch */
function nextPatch() {
    host_module_set_param("next_patch", "1");
    needsRedraw = true;
}

function prevPatch() {
    host_module_set_param("prev_patch", "1");
    needsRedraw = true;
}

/* Adjust octave */
function octaveUp() {
    if (octaveTranspose < 4) {
        octaveTranspose++;
        host_module_set_param("octave_transpose", String(octaveTranspose));
        needsRedraw = true;
    }
}

function octaveDown() {
    if (octaveTranspose > -4) {
        octaveTranspose--;
        host_module_set_param("octave_transpose", String(octaveTranspose));
        needsRedraw = true;
    }
}

/* === Required module UI callbacks === */

globalThis.init = function() {
    console.log("Signal Chain UI initializing...");
    needsRedraw = true;
    console.log("Signal Chain UI ready");
};

globalThis.tick = function() {
    /* Set default octave on first tick */
    if (!octaveInitialized) {
        host_module_set_param("octave_transpose", "-2");
        octaveTranspose = -2;
        octaveInitialized = true;
    }

    /* Update state from DSP */
    updateState();

    /* Rate-limited redraw */
    tickCount++;
    if (needsRedraw || tickCount >= REDRAW_INTERVAL) {
        drawUI();
        tickCount = 0;
        needsRedraw = false;
    }
};

globalThis.onMidiMessageInternal = function(data) {
    const useJsMidiFx = midiFxJsChain.length > 0;
    const isCap = isCapacitiveTouchMessage(data);
    if (!useJsMidiFx && isCap) return;
    if (useJsMidiFx && isCap && !rawMidi) return;

    const status = data[0] & 0xF0;

    if (status === 0xB0) {
        handleCC(data[1], data[2]);
        return;
    }

    if (useJsMidiFx) {
        if (!midiSourceAllowed("internal")) return;
        processMidiFx([data[0], data[1], data[2]], "internal");
    }

    /* Handle CC messages */
    /* Note activity triggers redraw for visual feedback */
    if (status === 0x90 || status === 0x80) {
        needsRedraw = true;
    }
};

globalThis.onMidiMessageExternal = function(data) {
    if (midiFxJsChain.length === 0) return;
    if (!midiSourceAllowed("external")) return;
    processMidiFx([data[0], data[1], data[2]], "external");
};
