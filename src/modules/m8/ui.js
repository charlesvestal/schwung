/*
 * M8 Launchpad Pro Emulator Module
 *
 * Emulates a Novation Launchpad Pro for the Dirtywave M8.
 * Maps Move pads/buttons to LPP protocol and handles bidirectional MIDI.
 */

import {
    MoveMenu, MoveBack, MoveCapture, MoveShift,
    MoveMainButton, MoveMainTouch,
    MovePlay, MoveRec, MoveLoop, MoveMute, MoveUndo,
    MovePad32, MidiClock
} from '../../shared/constants.mjs';

import { isCapacitiveTouch } from '../../shared/input_filter.mjs';

/* LPP note layout (10x10 grid) */
const lppNotes = [
    90, 91, 92, 93, 94, 95, 96, 97, 98, 99,
    80, 81, 82, 83, 84, 85, 86, 87, 88, 89,
    70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
    60, 61, 62, 63, 64, 65, 66, 67, 68, 69,
    50, 51, 52, 53, 54, 55, 56, 57, 58, 59,
    40, 41, 42, 43, 44, 45, 46, 47, 48, 49,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    101, 102, 103, 104, 105, 106, 107, 108,
    1, 2, 3, 4, 5, 6, 7, 8
];

const lppNoteValueMap = new Map([...lppNotes.map((a) => [a, [0, 0, 0]])]);

/* Move control to LPP note mapping (top view) */
const moveControlToLppNoteMapTop = new Map([
    [55, 80], [54, 70], [62, 91], [63, 92], [85, 20],
    [43, 89], [42, 79], [41, 69], [40, 59],
    [50, 94], [49, 90], [119, 60], [51, 93], [52, 97],
    [88, 2], [56, 1], [86, 10], [60, 50], [58, 3],
    [118, 98], [99, 99]
]);

const lppNoteToMoveControlMapTop = new Map([...moveControlToLppNoteMapTop.entries()].map((a) => [a[1], a[0]]));

/* Move control to LPP note mapping (bottom view) */
const moveControlToLppNoteMapBottom = new Map([
    [55, 80], [54, 70], [62, 91], [63, 92], [85, 20],
    [43, 49], [42, 39], [41, 29], [40, 19],
    [50, 94], [49, 90], [119, 60], [51, 93], [52, 97],
    [88, 2], [56, 1], [86, 10], [60, 50], [58, 3],
    [118, 98], [99, 99]
]);

const lppNoteToMoveControlMapBottom = new Map([...moveControlToLppNoteMapBottom.entries()].map((a) => [a[1], a[0]]));

/* LPP pad to Move pad mapping (top view) */
const lppPadToMovePadMapTop = new Map([
    [81, 92], [82, 93], [83, 94], [84, 95], [85, 96], [86, 97], [87, 98], [88, 99],
    [71, 84], [72, 85], [73, 86], [74, 87], [75, 88], [76, 89], [77, 90], [78, 91],
    [61, 76], [62, 77], [63, 78], [64, 79], [65, 80], [66, 81], [67, 82], [68, 83],
    [51, 68], [52, 69], [53, 70], [54, 71], [55, 72], [56, 73], [57, 74], [58, 75],
    [101, 16], [102, 18], [103, 20], [104, 22], [105, 24], [106, 26], [107, 28], [108, 30]
]);

const moveToLppPadMapTop = new Map([...lppPadToMovePadMapTop.entries()].map((a) => [a[1], a[0]]));

/* LPP pad to Move pad mapping (bottom view) */
const lppPadToMovePadMapBottom = new Map([
    [41, 92], [42, 93], [43, 94], [44, 95], [45, 96], [46, 97], [47, 98], [48, 99],
    [31, 84], [32, 85], [33, 86], [34, 87], [35, 88], [36, 89], [37, 90], [38, 91],
    [21, 76], [22, 77], [23, 78], [24, 79], [25, 80], [26, 81], [27, 82], [28, 83],
    [11, 68], [12, 69], [13, 70], [14, 71], [15, 72], [16, 73], [17, 74], [18, 75],
    [101, 16], [102, 18], [103, 20], [104, 22], [105, 24], [106, 26], [107, 28], [108, 30]
]);

const moveToLppPadMapBottom = new Map([...lppPadToMovePadMapBottom.entries()].map((a) => [a[1], a[0]]));

/* M8-specific colors for LPP color translation */
const light_grey = 0x7c;
const dim_grey = 0x10;
const green = 0x7e;
const navy = 0x7d;
const sky = 0x5f;
const black = 0x00;

/* Alias imported constants for local usage */
const moveLOGO = MovePad32;
const moveMENU = MoveMenu;
const moveBACK = MoveBack;
const moveCAP = MoveCapture;
const moveSHIFT = MoveShift;
const moveWHEEL = MoveMainButton;
const movePLAY = MovePlay;
const moveREC = MoveRec;
const moveLOOP = MoveLoop;
const moveMUTE = MoveMute;
const moveUNDO = MoveUndo;
const moveWHEELTouch = MoveMainTouch;

/* Color mapping */
const lppColorToMoveColorMap = new Map([
    [0x15, green], [0x17, 0x20], [0x1, light_grey], [0x05, 0x7f], [0x03, 0x7a], [0x4e, 0x5f],
    [0x47, 0x6d], [0x13, 0x5a], [0x27, 0x5f], [0x2b, 0x63], [0x16, 0x55]
]);

const lppColorToMoveMonoMap = new Map([
    [0x05, 0x7f], [0x78, 0x7f], [0x01, 0x10], [0x07, 0x0f]
]);

/* State */
let showingTop = true;
let shiftHeld = false;
let liveMode = false;
let isPlaying = false;
let currentView = moveBACK;
let wheelClicked = false;
let sysexBuffer = [];
const m8InitSysex = [0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7];

/* Display state */
let line1 = "M8 LPP Emulator";
let line2 = "Waiting for M8";
let line3 = "";
let line4 = "";

function drawUI() {
    clear_screen();
    print(2, 2, line1, 1);
    print(2, 18, line2, 1);
    print(2, 34, line3, 1);
    print(2, 50, line4, 1);
}

function displayMessage(l1, l2, l3, l4) {
    if (l1) line1 = l1;
    if (l2) line2 = l2;
    if (l3 !== undefined) line3 = l3;
    if (l4 !== undefined) line4 = l4;
}

function arraysAreEqual(array1, array2) {
    if (array1.length !== array2.length) return false;
    for (let i = 0; i < array1.length; i++) {
        if (array1[i] !== array2[i]) return false;
    }
    return true;
}

function updateMovePadsToMatchLpp() {
    let activeMoveToLppPadMap = showingTop ? moveToLppPadMapTop : moveToLppPadMapBottom;
    for (let [movePad, lppPad] of activeMoveToLppPadMap.entries()) {
        let data = lppNoteValueMap.get(lppPad);
        if (data) globalThis.onMidiMessageExternal(data);
    }
}

function updateMoveViewPulse() {
    move_midi_internal_send([0x0b, 0xB0, moveBACK, dim_grey]);
    move_midi_internal_send([0x0b, 0xB0, moveMENU, dim_grey]);
    move_midi_internal_send([0x0b, 0xB0, moveCAP, dim_grey]);
    move_midi_internal_send([0x0b, 0xB0, currentView, light_grey]);
    if (!showingTop) {
        move_midi_internal_send([0x0b, 0xBA, currentView, black]);
    }
}

function updatePLAYLed() {
    if (!liveMode && !isPlaying) move_midi_internal_send([0x0b, 0xB0, movePLAY, light_grey]);
    if (!liveMode && isPlaying) move_midi_internal_send([0x0b, 0xB0, movePLAY, green]);
    if (liveMode && !isPlaying) move_midi_internal_send([0x0b, 0xB0, movePLAY, sky]);
    if (liveMode && isPlaying) move_midi_internal_send([0x0b, 0xB0, movePLAY, navy]);
}

function initLPP() {
    let out_cable = 2;
    let LPPInitSysex = [
        out_cable << 4 | 0x4, 0xF0, 126, 0,
        out_cable << 4 | 0x4, 6, 2, 0,
        out_cable << 4 | 0x4, 32, 41, 0x00,
        out_cable << 4 | 0x4, 0x00, 0x00, 0x00,
        out_cable << 4 | 0x4, 0x00, 0x00, 0x00,
        out_cable << 4 | 0x6, 0x00, 0xF7, 0x0
    ];
    move_midi_external_send(LPPInitSysex);
    showingTop = true;
    displayMessage("M8 LPP Emulator", "M8 connected", "", "");
}

/* External MIDI handler (from M8) */
globalThis.onMidiMessageExternal = function (data) {
    if (data[0] === MidiClock) return;

    let value = data[0];
    let maskedValue = (value & 0xf0);
    let noteOn = maskedValue === 0x90;
    let noteOff = maskedValue === 0x80;

    /* Handle sysex */
    let sysexStart = value === 0xF0;
    let sysexEnd = data[2] === 0xF7;

    if (sysexStart) {
        sysexBuffer = [];
        sysexBuffer.push(...data);
        return;
    }
    if (sysexBuffer.length && !sysexEnd) {
        sysexBuffer.push(...data);
        return;
    }
    if (sysexEnd) {
        sysexBuffer.push(...data);
        if (arraysAreEqual(sysexBuffer, m8InitSysex)) {
            initLPP();
        }
        sysexBuffer = [];
        return;
    }

    if (!(noteOn || noteOff)) return;

    let lppNoteNumber = data[1];
    let lppVelocity = data[2];

    lppNoteValueMap.set(lppNoteNumber, [...data]);

    let activeLppToMovePadMap = showingTop ? lppPadToMovePadMapTop : lppPadToMovePadMapBottom;
    let moveNoteNumber = activeLppToMovePadMap.get(lppNoteNumber);
    let moveVelocity = lppColorToMoveColorMap.get(lppVelocity) ?? lppVelocity;

    if (moveNoteNumber) {
        if (value === 0x91 && moveVelocity != 0) {
            move_midi_internal_send([0x09, 0x9f, moveNoteNumber, moveVelocity]);
        } else {
            move_midi_internal_send([(maskedValue / 16), maskedValue, moveNoteNumber, moveVelocity]);
            if (value === 0x92 && moveVelocity != 0) {
                move_midi_internal_send([0x09, 0x9a, moveNoteNumber, light_grey]);
            }
        }
        return;
    }

    let activeLppToMoveControlMap = showingTop ? lppNoteToMoveControlMapTop : lppNoteToMoveControlMapBottom;
    let moveControlNumber = activeLppToMoveControlMap.get(lppNoteNumber);

    if (moveControlNumber === moveLOGO) {
        liveMode = moveVelocity > 0;
        updatePLAYLed();
    }

    if (moveControlNumber === movePLAY) {
        isPlaying = moveVelocity === green;
        updatePLAYLed();
        return;
    }

    if (moveControlNumber === moveLOOP || moveControlNumber === moveMUTE || moveControlNumber === moveUNDO) {
        moveVelocity = lppColorToMoveMonoMap.get(lppVelocity) ?? lppVelocity;
    }

    if (moveControlNumber) {
        move_midi_internal_send([0x0b, 0xB0, moveControlNumber, moveVelocity]);
        if (value === 0x91) {
            move_midi_internal_send([0x0b, 0xbe, moveControlNumber, black]);
        }
    }
};

/* Internal MIDI handler (from Move) */
globalThis.onMidiMessageInternal = function (data) {
    const isNote = data[0] === 0x80 || data[0] === 0x90;
    const isCC = data[0] === 0xb0;
    const isAt = data[0] === 0xa0;

    /* Filter capacitive touch (notes < 10, except wheel touch for navigation) */
    if (isNote && isCapacitiveTouch(data[1]) && data[1] !== moveWHEELTouch) {
        return;
    }

    if (isAt) return; /* Ignore aftertouch */

    let activeMoveToLppPadMap = showingTop ? moveToLppPadMapTop : moveToLppPadMapBottom;

    if (isNote) {
        let moveNoteNumber = data[1];

        /* Wheel touch toggles top/bottom view */
        if (moveNoteNumber === moveWHEELTouch && data[2] == 127) {
            showingTop = !showingTop;
            updateMovePadsToMatchLpp();
            updateMoveViewPulse();
            return;
        }

        if (moveNoteNumber === moveWHEELTouch && data[2] == 0) {
            if (!wheelClicked) {
                showingTop = !showingTop;
                updateMovePadsToMatchLpp();
                updateMoveViewPulse();
            }
            wheelClicked = false;
            return;
        }

        let lppNote = activeMoveToLppPadMap.get(moveNoteNumber);

        if (!lppNote) return;

        let moveVelocity = data[2] * 4;
        if (moveVelocity > 127) moveVelocity = 127;

        move_midi_external_send([2 << 4 | (data[0] / 0xF), data[0], lppNote, moveVelocity]);
        return;
    }

    if (isCC) {
        let moveControlNumber = data[1];
        let activeMoveControlToLppNoteMap = showingTop ? moveControlToLppNoteMapTop : moveControlToLppNoteMapBottom;
        let lppNote = activeMoveControlToLppNoteMap.get(moveControlNumber);

        /* Store current view */
        if (moveControlNumber === moveBACK || moveControlNumber === moveMENU || moveControlNumber === moveCAP) {
            currentView = moveControlNumber;
            updateMoveViewPulse();
        }

        /* Note: Shift+Wheel exit is handled at host level */

        /* Wheel click */
        if (moveControlNumber === moveWHEEL && data[2] === 0x7f) {
            wheelClicked = true;
            return;
        }

        if (!lppNote) return;

        let pressed = data[2] === 127;

        if (pressed) {
            if (moveControlNumber === moveSHIFT) {
                shiftHeld = true;
                displayMessage(undefined, "Shift held", "", "");
            }
            move_midi_external_send([2 << 4 | 0x9, 0x90, lppNote, 100]);
        } else {
            if (moveControlNumber === moveSHIFT) {
                shiftHeld = false;
                displayMessage(undefined, "M8 connected", "", "");
            }
            move_midi_external_send([2 << 4 | 0x8, 0x80, lppNote, 0]);
        }
    }
};

globalThis.init = function () {
    console.log("M8 LPP Emulator module starting...");
    displayMessage("M8 LPP Emulator", "Waiting for M8", "to connect", "");
};

globalThis.tick = function () {
    drawUI();
};
