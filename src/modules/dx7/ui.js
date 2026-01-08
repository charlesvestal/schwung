/*
 * DX7 Synth Module UI
 *
 * Provides UI for the DX7 FM synthesizer module.
 * Handles preset selection, octave transpose, and display updates.
 */

/* State */
let currentPreset = 0;
let presetCount = 32;  /* Standard DX7 bank has 32 presets */
let patchName = "Init";
let algorithm = 1;
let octaveTranspose = 0;
let polyphony = 0;

/* Move hardware constants */
const CC_JOG_WHEEL = 14;
const CC_JOG_CLICK = 3;
const CC_SHIFT = 49;
const CC_MENU = 50;
const CC_LEFT = 62;
const CC_RIGHT = 63;
const CC_PLUS = 55;
const CC_MINUS = 54;

let shiftHeld = false;
let needsRedraw = true;

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

    /* Navigation hint */
    print(2, 54, "Jog:Preset  +/-:Oct", 1);

    needsRedraw = false;
}

/* Change preset */
function setPreset(index) {
    if (index < 0) index = presetCount - 1;
    if (index >= presetCount) index = 0;

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
    octaveTranspose += delta;
    if (octaveTranspose < -4) octaveTranspose = -4;
    if (octaveTranspose > 4) octaveTranspose = 4;

    host_module_set_param("octave_transpose", String(octaveTranspose));

    needsRedraw = true;
    console.log(`DX7: Octave transpose: ${octaveTranspose}`);
}

/* Handle CC messages */
function handleCC(cc, value) {
    if (cc === CC_SHIFT) {
        shiftHeld = (value > 0);
        return true;
    }

    if (cc === CC_JOG_CLICK && shiftHeld) {
        console.log("Shift+Wheel - exit");
        exit();
        return true;
    }

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

let tickCount = 0;
const REDRAW_INTERVAL = 10;

globalThis.tick = function() {
    tickCount++;

    /* Update polyphony */
    const poly = host_module_get_param("polyphony");
    if (poly) {
        const newPoly = parseInt(poly) || 0;
        if (newPoly !== polyphony) {
            polyphony = newPoly;
            needsRedraw = true;
        }
    }

    if (needsRedraw || (tickCount % REDRAW_INTERVAL === 0)) {
        drawUI();
    }
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    const isNote = status === 0x90 || status === 0x80;

    /* Filter capacitive touch */
    if (isNote && data[1] < 10) {
        return;
    }

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
