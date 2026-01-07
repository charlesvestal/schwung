/*
 * SF2 Synth Module UI
 *
 * Provides UI for the SF2 SoundFont synthesizer module.
 * Handles preset selection, octave transpose, and display updates.
 */

/* State */
let currentPreset = 0;
let presetCount = 128;  /* Will be updated from DSP */
let presetName = "Piano";
let soundfontName = "instrument.sf2";
let octaveTranspose = 0;
let polyphony = 0;

/* Move hardware constants */
const CC_JOG_WHEEL = 14;
const CC_JOG_CLICK = 3;  /* Jog wheel click (as CC) */
const CC_SHIFT = 49;
const CC_MENU = 50;
const CC_LEFT = 62;
const CC_RIGHT = 63;
const CC_PLUS = 55;  /* Plus button for octave up */
const CC_MINUS = 54; /* Minus button for octave down */

/* Note range for Move pads */
const PAD_NOTE_MIN = 68;
const PAD_NOTE_MAX = 99;

let shiftHeld = false;
let needsRedraw = true;

/* Display constants */
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;

/* Draw the UI */
function drawUI() {
    clear_screen();

    /* Title bar */
    print(2, 2, "SF2 Synth", 1);
    fill_rect(0, 12, SCREEN_WIDTH, 1, 1);

    /* Soundfont name */
    print(2, 16, soundfontName.substring(0, 20), 1);

    /* Preset info */
    print(2, 28, `Preset: ${currentPreset}`, 1);
    print(2, 38, presetName.substring(0, 18), 1);

    /* Octave and polyphony */
    const octStr = octaveTranspose >= 0 ? `+${octaveTranspose}` : `${octaveTranspose}`;
    print(2, 50, `Oct:${octStr}  Poly:${polyphony}`, 1);

    /* Navigation hint */
    print(80, 50, "<L R>", 1);

    needsRedraw = false;
}

/* Change preset */
function setPreset(index) {
    if (index < 0) index = presetCount - 1;
    if (index >= presetCount) index = 0;

    currentPreset = index;
    host_module_set_param("preset", String(currentPreset));

    /* Try to get preset name from DSP */
    const name = host_module_get_param("preset_name");
    if (name) {
        presetName = name;
    } else {
        presetName = `Preset ${currentPreset}`;
    }

    needsRedraw = true;
    console.log(`SF2: Preset changed to ${currentPreset}: ${presetName}`);
}

/* Change octave */
function setOctave(delta) {
    octaveTranspose += delta;
    if (octaveTranspose < -4) octaveTranspose = -4;
    if (octaveTranspose > 4) octaveTranspose = 4;

    /* Sync with DSP */
    host_module_set_param("octave_transpose", String(octaveTranspose));

    needsRedraw = true;
    console.log(`SF2: Octave transpose: ${octaveTranspose}`);
}

/* Handle CC messages */
function handleCC(cc, value) {
    /* Track shift state */
    if (cc === CC_SHIFT) {
        shiftHeld = (value > 0);
        return true;
    }

    /* Shift+Jog click exits Move Anything */
    if (cc === CC_JOG_CLICK && shiftHeld) {
        console.log("Shift+Wheel - exit");
        exit();
        return true;
    }

    /* Preset navigation with left/right buttons */
    if (cc === CC_LEFT && value > 0) {
        setPreset(currentPreset - 1);
        return true;
    }
    if (cc === CC_RIGHT && value > 0) {
        setPreset(currentPreset + 1);
        return true;
    }

    /* Octave with up/down (plus/minus) */
    if (cc === CC_PLUS && value > 0) {
        setOctave(1);
        return true;
    }
    if (cc === CC_MINUS && value > 0) {
        setOctave(-1);
        return true;
    }

    /* Jog wheel for preset selection */
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

/* Transpose note based on octave setting */
function transposeNote(note) {
    return note + (octaveTranspose * 12);
}

/* Forward note to DSP with transpose applied */
function forwardNote(status, note, velocity) {
    /* Only transpose pad notes */
    if (note >= PAD_NOTE_MIN && note <= PAD_NOTE_MAX) {
        note = transposeNote(note);
        /* Clamp to valid MIDI range */
        if (note < 0) note = 0;
        if (note > 127) note = 127;
    }

    /* Note is forwarded to DSP by host automatically.
     * We just need to handle the transposition here.
     * Actually, since the host routes MIDI directly to DSP,
     * we need to send a param to tell DSP about transpose. */
    host_module_set_param("octave_transpose", String(octaveTranspose));
}

/* === Required module UI callbacks === */

globalThis.init = function() {
    console.log("SF2 UI initializing...");

    /* Get initial state from DSP */
    const sf = host_module_get_param("soundfont_name");
    if (sf) soundfontName = sf;

    const pc = host_module_get_param("preset_count");
    if (pc) presetCount = parseInt(pc) || 128;

    const pn = host_module_get_param("preset_name");
    if (pn) presetName = pn;

    const cp = host_module_get_param("preset");
    if (cp) currentPreset = parseInt(cp) || 0;

    needsRedraw = true;
    console.log(`SF2 UI ready: ${soundfontName}, ${presetCount} presets`);
};

let tickCount = 0;
const REDRAW_INTERVAL = 10; /* Redraw every N ticks */

globalThis.tick = function() {
    tickCount++;

    /* Update polyphony from DSP */
    const poly = host_module_get_param("polyphony");
    if (poly) {
        const newPoly = parseInt(poly) || 0;
        if (newPoly !== polyphony) {
            polyphony = newPoly;
            needsRedraw = true;
        }
    }

    /* Periodic redraw to keep UI responsive */
    if (needsRedraw || (tickCount % REDRAW_INTERVAL === 0)) {
        drawUI();
    }
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    const isNote = status === 0x90 || status === 0x80;

    /* Filter capacitive touch events from knobs (notes with data[1] < 10) */
    if (isNote && data[1] < 10) {
        return; /* Ignore capacitive touch */
    }

    if (status === 0xB0) {
        /* CC - handle UI controls */
        if (handleCC(data[1], data[2])) {
            return; /* Consumed by UI */
        }
    } else if (isNote) {
        /* Note - apply transpose */
        forwardNote(status, data[1], data[2]);
        needsRedraw = true;
    }

    /* All MIDI is also routed to DSP by host */
};

globalThis.onMidiMessageExternal = function(data) {
    /* External MIDI goes directly to DSP via host */
};
