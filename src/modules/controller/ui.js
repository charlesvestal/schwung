/*
 * MIDI Controller Module
 *
 * Custom MIDI controller with 16 banks and configurable pad/knob mappings.
 * Use step buttons to switch banks, pads send configurable notes.
 * Jog wheel adjusts octave (+/-4 octaves).
 * Knobs send CC values (absolute 0-127).
 */

import {
    Black, White, LightGrey, Red, Blue,
    MidiNoteOn, MidiNoteOff, MidiCC,
    MoveShift, MoveMainKnob, MoveUp, MoveDown,
    MovePads, MoveSteps,
    MoveKnob1, MoveKnob8, MoveMaster
} from '/data/UserData/move-anything/shared/constants.mjs';

import {
    isNoiseMessage, isCapacitiveTouchMessage,
    setLED, clearAllLEDs, decodeDelta
} from '/data/UserData/move-anything/shared/input_filter.mjs';

/* State */
let bank = 0;
let shiftHeld = false;
let octaveShift = 0;  // Range: -4 to +4 (in octaves)

/* Knob CC state - absolute values 0-127 for each knob */
let knobValues = [64, 64, 64, 64, 64, 64, 64, 64]; // Start at center

/* Progressive LED init - spread LED setup over multiple frames to avoid buffer overflow */
let ledInitPending = false;
let ledInitIndex = 0;
const LEDS_PER_FRAME = 8;  /* Send 8 LED messages per frame */

/* Display state */
let line1 = "MIDI Controller";
let line2 = "";
let line3 = "";
let line4 = "";

/* Default pad configuration: [baseNote, color, baseName] - C notes are brighter */
const defaultPadConfig = {
    68: [36, White, "C1"],       /* C = bright */
    69: [37, LightGrey, "C#1"],
    70: [38, LightGrey, "D1"],
    71: [39, LightGrey, "D#1"],
    72: [40, LightGrey, "E1"],
    73: [41, LightGrey, "F1"],
    74: [42, LightGrey, "F#1"],
    75: [43, LightGrey, "G1"],
    76: [44, LightGrey, "G#1"],
    77: [45, LightGrey, "A1"],
    78: [46, LightGrey, "A#1"],
    79: [47, LightGrey, "B1"],
    80: [48, White, "C2"],       /* C = bright */
    81: [49, LightGrey, "C#2"],
    82: [50, LightGrey, "D2"],
    83: [51, LightGrey, "D#2"],
    84: [52, LightGrey, "E2"],
    85: [53, LightGrey, "F2"],
    86: [54, LightGrey, "F#2"],
    87: [55, LightGrey, "G2"],
    88: [56, LightGrey, "G#2"],
    89: [57, LightGrey, "A2"],
    90: [58, LightGrey, "A#2"],
    91: [59, LightGrey, "B2"],
    92: [60, White, "C3"],       /* C = bright */
    93: [61, LightGrey, "C#3"],
    94: [62, LightGrey, "D3"],
    95: [63, LightGrey, "D#3"],
    96: [64, LightGrey, "E3"],
    97: [65, LightGrey, "F3"],
    98: [66, LightGrey, "F#3"],
    99: [67, LightGrey, "G3"]
};

/* Bank storage */
let padBanks = [];
padBanks[0] = JSON.parse(JSON.stringify(defaultPadConfig));

/* Note names for display */
const noteNames = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

function getNoteDisplay(midiNote) {
    const octave = Math.floor(midiNote / 12) - 1;
    const name = noteNames[midiNote % 12];
    return `${name}${octave}`;
}

function drawUI() {
    clear_screen();
    print(2, 2, line1, 1);
    print(2, 18, line2, 1);
    print(2, 34, line3, 1);
    print(2, 50, line4, 1);
}

function displayMessage(l1, l2, l3, l4) {
    if (l1 !== undefined) line1 = l1;
    if (l2 !== undefined) line2 = l2;
    if (l3 !== undefined) line3 = l3;
    if (l4 !== undefined) line4 = l4;
}

function clearLEDs() {
    clearAllLEDs();
}

function fillPads(pads) {
    for (const pad of MovePads) {
        if (pads[pad]) {
            setLED(pad, pads[pad][1]);
        }
    }
}

function getOctaveDisplay() {
    if (octaveShift === 0) return "Oct: 0";
    if (octaveShift > 0) return `Oct: +${octaveShift}`;
    return `Oct: ${octaveShift}`;
}

function updateStatusLine() {
    line4 = `Bank ${bank + 1}  ${getOctaveDisplay()}`;
}

globalThis.onMidiMessageExternal = function (data) {
    if (isNoiseMessage(data)) return;

    /* Pass through to Move LEDs */
    move_midi_internal_send([data[0] / 16, data[0], data[1], data[2]]);
};

globalThis.onMidiMessageInternal = function (data) {
    if (isNoiseMessage(data)) return;
    if (isCapacitiveTouchMessage(data)) return;

    const status = data[0];
    const d1 = data[1];
    const d2 = data[2];

    const isNote = status === MidiNoteOn || status === MidiNoteOff;
    const isNoteOn = status === MidiNoteOn;
    const isCC = status === MidiCC;

    if (isNote) {
        let note = d1;
        let velocity = d2;

        /* Bank switching via step buttons */
        if (MoveSteps.includes(note) && velocity === 127) {
            /* Clear previous bank LED */
            setLED(MoveSteps[bank], Black);
            /* Light new bank LED */
            setLED(note, White);

            bank = MoveSteps.indexOf(note);

            /* Create bank if doesn't exist */
            if (!padBanks[bank]) {
                padBanks[bank] = JSON.parse(JSON.stringify(defaultPadConfig));
            }

            fillPads(padBanks[bank]);
            displayMessage("MIDI Controller", `Bank ${bank + 1}`, "", "");
            updateStatusLine();
            return;
        }

        /* Handle pads */
        if (MovePads.includes(note)) {
            let pad = padBanks[bank][note];
            if (!pad) return;

            /* Apply octave shift to the note */
            let outputNote = pad[0] + (octaveShift * 12);
            /* Clamp to valid MIDI range */
            outputNote = Math.max(0, Math.min(127, outputNote));

            /* Send mapped MIDI note with octave shift */
            move_midi_external_send([2 << 4 | (status / 16), status, outputNote, velocity]);

            if (isNoteOn && velocity > 0) {
                setLED(note, White);
                displayMessage("MIDI Controller",
                    `Note ${outputNote} (${getNoteDisplay(outputNote)})`,
                    `Velocity ${velocity}`,
                    undefined);
                updateStatusLine();
            } else {
                setLED(note, pad[1]);
            }
            return;
        }
    }

    if (isCC) {
        let ccNumber = d1;
        let value = d2;

        /* Shift state tracking */
        if (ccNumber === MoveShift) {
            shiftHeld = value === 127;
            if (shiftHeld) {
                displayMessage(undefined, "Shift held", "", undefined);
            } else {
                displayMessage(undefined, "", "", undefined);
                updateStatusLine();
            }
            return;
        }

        /* Jog wheel for octave shift */
        if (ccNumber === MoveMainKnob) {
            const delta = decodeDelta(value);
            if (delta !== 0) {
                octaveShift = Math.max(-4, Math.min(4, octaveShift + delta));
                displayMessage(undefined, getOctaveDisplay(), "", undefined);
                updateStatusLine();
                /* Refresh pad LEDs to update C highlighting */
                ledInitPending = true;
                ledInitIndex = 0;
            }
            return;
        }

        /* Up/Down buttons for octave shift */
        if (ccNumber === MoveUp && value > 0) {
            octaveShift = Math.min(4, octaveShift + 1);
            displayMessage(undefined, getOctaveDisplay(), "", undefined);
            updateStatusLine();
            /* Refresh pad LEDs to update C highlighting */
            ledInitPending = true;
            ledInitIndex = 0;
            return;
        }
        if (ccNumber === MoveDown && value > 0) {
            octaveShift = Math.max(-4, octaveShift - 1);
            displayMessage(undefined, getOctaveDisplay(), "", undefined);
            updateStatusLine();
            /* Refresh pad LEDs to update C highlighting */
            ledInitPending = true;
            ledInitIndex = 0;
            return;
        }

        /* Knob CCs (71-78) - convert delta to absolute and send */
        if (ccNumber >= MoveKnob1 && ccNumber <= MoveKnob8) {
            const knobIndex = ccNumber - MoveKnob1;
            const delta = decodeDelta(value);

            /* Update absolute value */
            knobValues[knobIndex] = Math.max(0, Math.min(127, knobValues[knobIndex] + delta));

            /* Map to CCs 1-8 for external output (common CC assignments) */
            const outputCC = knobIndex + 1;
            move_midi_external_send([2 << 4 | 0x0b, MidiCC, outputCC, knobValues[knobIndex]]);

            let direction = delta > 0 ? "+" : "-";
            displayMessage(undefined, `CC ${outputCC}`, `${knobValues[knobIndex]} (${direction}${Math.abs(delta)})`, undefined);
            updateStatusLine();
            return;
        }

        /* Master knob (CC 79) - send as CC 9 */
        if (ccNumber === MoveMaster) {
            const delta = decodeDelta(value);
            /* Use knobValues[8] for master, but we only have 8 elements, so use a separate var */
            /* For now, just send relative delta converted to absolute */
            move_midi_external_send([2 << 4 | 0x0b, MidiCC, 9, value]);
            let direction = delta > 0 ? "+" : "-";
            displayMessage(undefined, "Master", direction.repeat(Math.abs(delta)), undefined);
            updateStatusLine();
            return;
        }

        /* Forward other CCs as-is */
        move_midi_external_send([2 << 4 | 0x0b, MidiCC, ccNumber, value]);
    }
};

/* Get color for a pad based on whether its output note (with octave shift) is a C */
function getPadColor(pad) {
    const padConfig = padBanks[bank][pad];
    if (!padConfig) return LightGrey;

    const baseNote = padConfig[0];
    const outputNote = baseNote + (octaveShift * 12);

    /* C notes are 0, 12, 24, 36, 48, 60, 72... (divisible by 12) */
    if (outputNote >= 0 && outputNote <= 127 && outputNote % 12 === 0) {
        return White;  /* C notes are bright */
    }
    return LightGrey;
}

/* Progressive LED setup - called each tick until done */
function setupLedBatch() {
    /* LEDs to set: step 0 (bank indicator) + 32 pads */
    const ledsToSet = [
        { note: MoveSteps[bank], color: White }  /* Current bank indicator */
    ];
    /* Add all pads with dynamic colors based on octave shift */
    for (const pad of MovePads) {
        if (padBanks[bank][pad]) {
            ledsToSet.push({ note: pad, color: getPadColor(pad) });
        }
    }

    const start = ledInitIndex;
    const end = Math.min(start + LEDS_PER_FRAME, ledsToSet.length);

    for (let i = start; i < end; i++) {
        setLED(ledsToSet[i].note, ledsToSet[i].color);
    }

    ledInitIndex = end;
    if (ledInitIndex >= ledsToSet.length) {
        ledInitPending = false;
        ledInitIndex = 0;
    }
}

globalThis.init = function () {
    console.log("MIDI Controller module starting...");

    displayMessage("MIDI Controller", "", "", "");
    updateStatusLine();

    /* Note: LEDs are cleared by host before loading overtake module.
     * Use progressive LED init to avoid buffer overflow. */
    ledInitPending = true;
    ledInitIndex = 0;
};

globalThis.tick = function () {
    /* Continue progressive LED setup if pending */
    if (ledInitPending) {
        setupLedBatch();
    }

    drawUI();
};
