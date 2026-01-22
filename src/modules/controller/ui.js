/*
 * MIDI Controller Module
 *
 * Custom MIDI controller with 16 banks and configurable pad/knob mappings.
 * Use step buttons to switch banks, pads send configurable notes.
 */

import {
    Black, White, LightGrey,
    MidiNoteOn, MidiNoteOff, MidiCC,
    MoveShift, MoveMainKnob,
    MovePads, MoveSteps,
    MoveKnob1, MoveMaster
} from '/data/UserData/move-anything/shared/constants.mjs';

import {
    isNoiseMessage, isCapacitiveTouchMessage,
    setLED, clearAllLEDs
} from '/data/UserData/move-anything/shared/input_filter.mjs';

/* State */
let bank = 0;
let shiftHeld = false;

/* Display state */
let line1 = "MIDI Controller";
let line2 = "";
let line3 = "";
let line4 = "";

/* Default pad configuration: [note, color, name] */
const defaultPadConfig = {
    68: [36, LightGrey, "C1"],
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
    80: [48, LightGrey, "C2"],
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
    92: [60, LightGrey, "C3"],
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

globalThis.onMidiMessageExternal = function (data) {
    if (isNoiseMessage(data)) return;

    /* Pass through to Move LEDs */
    move_midi_internal_send([data[0] / 16, data[0], data[1], data[2]]);
};

globalThis.onMidiMessageInternal = function (data) {
    if (isNoiseMessage(data)) return;
    if (isCapacitiveTouchMessage(data)) return;

    const isNote = data[0] === MidiNoteOn || data[0] === MidiNoteOff;
    const isNoteOn = data[0] === MidiNoteOn;
    const isCC = data[0] === MidiCC;

    if (isNote) {
        let note = data[1];
        let velocity = data[2];

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
            return;
        }

        /* Handle pads */
        if (MovePads.includes(note)) {
            let pad = padBanks[bank][note];
            if (!pad) return;

            /* Send mapped MIDI note */
            move_midi_external_send([2 << 4 | (data[0] / 16), data[0], pad[0], velocity]);

            if (isNoteOn && velocity > 0) {
                setLED(note, White);
                displayMessage("MIDI Controller",
                    `Note ${pad[0]} (${pad[2]})`,
                    `Velocity ${velocity}`,
                    `Bank ${bank + 1}`);
            } else {
                setLED(note, pad[1]);
            }
            return;
        }
    }

    if (isCC) {
        let ccNumber = data[1];
        let value = data[2];

        /* Shift state tracking */
        if (ccNumber === MoveShift) {
            shiftHeld = value === 127;
            if (shiftHeld) {
                displayMessage(undefined, "Shift held", "", undefined);
            } else {
                displayMessage(undefined, `Bank ${bank + 1}`, "", undefined);
            }
            return;
        }

        /* Note: Shift+Wheel exit is handled at host level */

        /* Forward other CCs */
        move_midi_external_send([2 << 4 | 0x0b, MidiCC, ccNumber, value]);

        /* Display knob movements */
        if (ccNumber >= MoveKnob1 && ccNumber <= MoveMaster) {
            let direction = value === 1 ? "+++" : "---";
            displayMessage(undefined, `Knob ${ccNumber - MoveKnob1 + 1}`, direction, undefined);
        } else if (ccNumber === MoveMainKnob) {
            let direction = value === 1 ? "+++" : "---";
            displayMessage(undefined, "Main Knob", direction, undefined);
        }
    }
};

globalThis.init = function () {
    console.log("MIDI Controller module starting...");

    displayMessage("MIDI Controller", `Bank ${bank + 1}`, "", "");
    clearLEDs();

    /* Light first bank */
    setLED(MoveSteps[0], White);

    fillPads(padBanks[bank]);
};

globalThis.tick = function () {
    drawUI();
};
