import { aftertouchToModwheel } from "./aftertouch_to_modwheel.mjs";
import { handleMoveKnobs } from "./move_virtual_knobs.mjs";

// https://github.com/Ableton/push-interface/blob/main/doc/AbletonPush2MIDIDisplayInterface.asc#setting-led-colors

// https://fael-downloads-prod.focusrite.com/customer/prod/s3fs-public/downloads/LPP3_prog_ref_guide_200415.pdf


// content = [
//     "SHF", "<", ">", "SES", "NTE", "CHD", "CUS", "SEQ", "PRJ", "",
//     "^", "", "", "", "", "", "", "", "", ">",
//     "v", "", "", "", "", "", "", "", "", ">",
//     "CLR", "", "", "", "", "", "", "", "", ">",
//     "DUP", "", "", "", "", "", "", "", "", ">",
//     "OVR", "", "", "", "", "", "", "", "", ">",
//     "FXD", "", "", "", "", "", "", "", "", ">",
//     ">", "", "", "", "", "", "", "", "", ">",
//     "o", "", "", "", "", "", "", "", "", ">",
//     "-", "-", "-", "-", "-", "-", "-", "-",
//     "ARM", "MUT", "SOL", "VOL", "PAN", "SND", "DEV", "STO"
// ];

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

const lppNoteValueMap = new Map([...lppNotes.map((a)=>[a, [0,0,0]])]);

const moveControlToLppNoteMapTop = new Map([
    [55, 80],
    [54, 70],
    [62, 91],
    [63, 92],
    [85, 20],
    [43, 89], // move track 1 -> LPP track 1
    [42, 79], // move track 2 -> LPP track 2
    [41, 69], // move track 3 -> LPP track 3
    [40, 59], // move track 4 -> LPP track 4
    [50, 94],
    [49, 90],
    [119, 60],
    [51, 93],
    [52, 97],
    [88, 2],
    [56, 1],
    [86, 10],
    [60, 50],
    [58, 3],
    [118, 98],
    // [78, 99]
    // here to allow Novation Logo LED msg to pass
    [99, 99]
]);

const lppNoteToMoveControlMapTop = new Map([...moveControlToLppNoteMapTop.entries()].map((a) => [a[1], a[0]]));

const moveControlToLppNoteMapBottom = new Map([
    [55, 80],
    [54, 70],
    [62, 91],
    [63, 92],
    [85, 20],
    [43, 49], // move track 1 -> LPP track 5
    [42, 39], // move track 2 -> LPP track 6
    [41, 29], // move track 3 -> LPP track 7
    [40, 19], // move track 4 -> LPP track 8
    [50, 94],
    [49, 90],
    [119, 60],
    [51, 93],
    [52, 97],
    [88, 2],
    [56, 1],
    [86, 10],
    [60, 50],
    [58, 3],
    [118, 98],
    // [78, 99]
    // here to allow Novation Logo LED msg to pass
    [99, 99]
]);

const lppNoteToMoveControlMapBottom = new Map([...moveControlToLppNoteMapBottom.entries()].map((a) => [a[1], a[0]]));


const lppPadToMovePadMapTop = new Map([
    [81, 92], [82, 93], [83, 94], [84, 95], [85, 96], [86, 97], [87, 98], [88, 99],
    [71, 84], [72, 85], [73, 86], [74, 87], [75, 88], [76, 89], [77, 90], [78, 91],
    [61, 76], [62, 77], [63, 78], [64, 79], [65, 80], [66, 81], [67, 82], [68, 83],
    [51, 68], [52, 69], [53, 70], [54, 71], [55, 72], [56, 73], [57, 74], [58, 75],
    [101, 16], [102, 18], [103, 20], [104, 22], [105, 24], [106, 26], [107, 28], [108, 30]
])

const moveToLppPadMapTop = new Map([...lppPadToMovePadMapTop.entries()].map((a) => [a[1], a[0]]));


const lppPadToMovePadMapBottom = new Map([
    [41, 92], [42, 93], [43, 94], [44, 95], [45, 96], [46, 97], [47, 98], [48, 99],
    [31, 84], [32, 85], [33, 86], [34, 87], [35, 88], [36, 89], [37, 90], [38, 91],
    [21, 76], [22, 77], [23, 78], [24, 79], [25, 80], [26, 81], [27, 82], [28, 83],
    [11, 68], [12, 69], [13, 70], [14, 71], [15, 72], [16, 73], [17, 74], [18, 75],
    [101, 16], [102, 18], [103, 20], [104, 22], [105, 24], [106, 26], [107, 28], [108, 30]
])

const moveToLppPadMapBottom = new Map([...lppPadToMovePadMapBottom.entries()].map((a) => [a[1], a[0]]));

let showingTop = true;

const light_grey = 0x7c;
const dim_grey = 0x10;
const green = 0x7e;
const navy = 0x7d;
const sky = 0x5f;
const red = 0x7f;
const blue = 0x5f;
const azure = 0x63;
const white = 0x7a;
const pink = 0x6d;
const aqua = 0x5a;
const black = 0x00;
const lemonade = 0x6b;
const lime = 0x20;
const fern = 0x55;

const moveLOGO = 99;

const moveMENU = 50;
const moveBACK = 51;
const moveCAP = 52;
const moveSHIFT = 49;
const moveWHEEL = 3;
const movePLAY = 85;
const moveREC = 86;
const moveLOOP = 58;
const moveMUTE = 88;
const moveUNDO = 56;
const moveTRACK1 = 16;
const moveSAMPLE = 118;

const moveWHEELTouch = 9;

const lppColorToMoveColorMap = new Map([
    [0x15, green], [0x17, lime], [0x1, light_grey], [0x05, red], [0x03, white], [0x4e, blue],
    [0x47, pink], [0x13, aqua], [0x47, lemonade], [0x27, blue], [0x2b, azure], [0x16, fern]
]);

const moveColorToLppColorMap = new Map([...lppColorToMoveColorMap.entries()].map((a) => [a[1], a[0]]));

const lppColorToMoveMonoMap = new Map([
    [0x05, 0x7f], [0x78, 0x7f], [0x01, 0x10], [0x07, 0x0f]
]);

const initDone = 1000;
const stepDelay = 20;
let shiftHeld = false;
let liveMode = false;
let isPlaying = false;
let currentView = moveBACK;
let initStep = 0;
let wheelClicked = false;
let timeStart = new Date();

let lppDebugSuperlog = false;

function updateMovePadsToMatchLpp() {
    let activeMoveToLppPadMap = showingTop ? moveToLppPadMapTop : moveToLppPadMapBottom;

    console.log(activeMoveToLppPadMap);

    for (let [movePad, lppPad] of activeMoveToLppPadMap.entries()) {
        let data = lppNoteValueMap.get(lppPad);

        console.log(lppPad, data);
        
        globalThis.onMidiMessageExternal(data);
    }
}

function updateMoveViewPulse(){
    // pulse VIEW if showing botttom - reset other views to dim_grey
    move_midi_internal_send([0 << 4 | 0xb, 0xB0, moveBACK, dim_grey]);
    move_midi_internal_send([0 << 4 | 0xb, 0xB0, moveMENU, dim_grey]);
    move_midi_internal_send([0 << 4 | 0xb, 0xB0, moveCAP, dim_grey]);
    move_midi_internal_send([0 << 4 | 0xb, 0xB0, currentView, light_grey]);
    if (!showingTop) {
        move_midi_internal_send([0 << 4 | 0xb, 0xBA, currentView, black]);
    }
}

function arraysAreEqual(array1, array2) {
    if (array1.length !== array2.length) {
        return false;
    }

    for (let i=0; i<array1.length; i++) {
        if (array1[i] !== array2[i]) {
            return false;
        }
    }

    return true;
}

let sysexBuffer = [];
let m8InitiSysex = [0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7];

globalThis.onMidiMessageExternal = function (data) {
    if (data[0] == 0xf8) {
        // midi clock. Ignoring...
        return;
    }

    console.log(`onMidiMessageExternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);

    
    let value = data[0];
    let maskedValue = (value & 0xf0);
    
    let noteOn = maskedValue === 0x90;
    let noteOff = maskedValue === 0x80;
    
    
    // Better to do this in the C layer, but this is fine for now...
    let sysexStart = value === 0xF0;
    let sysexEnd = data[2] === 0xF7;
    if (sysexStart) {
        //[ "f0", "7e", "7f", "6", "1", "f7" ]
        sysexBuffer = [];
        sysexBuffer.push(...data);
        console.log("sysex start", sysexBuffer);
        return;
    }

    if (sysexBuffer.length && !sysexEnd) {
        sysexBuffer.push(...data);
        console.log("sysex continue", sysexBuffer);
        return;
    }

    if (sysexEnd) {
        sysexBuffer.push(...data);
        console.log("sysex end", sysexBuffer.map((byte)=>byte.toString(16)));

        console.log(arraysAreEqual(sysexBuffer, m8InitiSysex));

        if (arraysAreEqual(sysexBuffer, m8InitiSysex)) {
            initLPP();
        }
        sysexBuffer = [];
        return;
    }


    
    // console.log(value, maskedValue, noteOn, noteOff);

    
    if (!(noteOn || noteOff)) {
        console.log(`Got message from M8 that is not a note: ${data}`);
    }
    
    let lppNoteNumber = data[1];
    let lppVelocity = data[2];
    
    lppNoteValueMap.set(lppNoteNumber, [...data]);

    if (lppDebugSuperlog) {
        let padCount = 0;
        let padString = "";
        for (let [padNumber, midi] of lppNoteValueMap.entries()) {
            padString += `${padNumber}(${midi})\t`;
            padCount++;
            if (padCount % 10 === 0) {
                console.log(padString);
                padCount = 0;
                padString = "";
            }
        }

         console.log(padString);
    }

    let activeLppToMovePadMap = showingTop ? lppPadToMovePadMapTop : lppPadToMovePadMapBottom;

    let moveNoteNumber = activeLppToMovePadMap.get(lppNoteNumber);
    let moveVelocity = lppColorToMoveColorMap.get(lppVelocity) ?? lppVelocity;

    if (moveNoteNumber) {
        console.log(`Got note on value : ${value}`);
        if (value === 0x91 && moveVelocity != 0) {
            move_midi_internal_send([0 << 4 | (value / 16), 0x9f, moveNoteNumber, moveVelocity]);
        } else {
            move_midi_internal_send([0 << 4 | (maskedValue / 16), maskedValue, moveNoteNumber, moveVelocity]);
            if (value === 0x92 && moveVelocity != 0) {
                move_midi_internal_send([0 << 4 | (value / 16), 0x9a, moveNoteNumber, light_grey]);
            }
        }
        return;
    }

    let activeLppToMoveControlMap = showingTop ? lppNoteToMoveControlMapTop : lppNoteToMoveControlMapBottom;

    let moveControlNumber = activeLppToMoveControlMap.get(lppNoteNumber);

    // test for LIVE mode message
    if (moveControlNumber === moveLOGO) {
        if (moveVelocity > 0) {
            liveMode = true;
        } else {
            liveMode = false;
        }
        updatePLAYLed();
    }
    // test for PLAY state
    if (moveControlNumber === movePLAY) {
        if (moveVelocity === green) {
            isPlaying = true;
        } else {
            isPlaying = false;
        }
        updatePLAYLed();
        return;
    }

    // hack for white LED controls
    if (moveControlNumber === moveLOOP || moveControlNumber === moveMUTE || moveControlNumber === moveUNDO) {
        moveVelocity = lppColorToMoveMonoMap.get(lppVelocity) ?? lppVelocity;
    }

    if (moveControlNumber) {
        console.log(`Sending LPP note ${lppNoteNumber} velocity ${lppVelocity} to Move control ${moveControlNumber} value: ${moveVelocity}`);

        move_midi_internal_send([0 << 4 | 0xb, 0xB0, moveControlNumber, moveVelocity]);
        if (value === 0x91) {
            move_midi_internal_send([0 << 4 | 0xb, 0xbe, moveControlNumber, black]);
        }
        return;
    }

    console.log(`Unmapped LPP note: ${lppNoteNumber}`);

}

globalThis.onMidiMessageInternal = function (data) {

    let isNote = data[0] === 0x80 || data[0] === 0x90;
    let isCC = data[0] === 0xb0;
    let isAt = data[0] === 0xa0;

    if (isAt) {
        if (aftertouchToModwheel(data)) {
            return;
        }
    }

    console.log(`onMidiMessageInternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);
    if (!(isNote || isCC)) {
        console.log(`Move: unknown message:`, data);
        return;
    }

    let activeMoveToLppPadMap = showingTop ? moveToLppPadMapTop : moveToLppPadMapBottom;


    if (isNote) {
        let moveNoteNumber = data[1];

        if (moveNoteNumber === moveWHEELTouch && data[2] == 127) {
            showingTop = !showingTop;
            updateMovePadsToMatchLpp();
            updateMoveViewPulse();
            return;
        }

        if (moveNoteNumber === moveWHEELTouch && data[2] == 0) {
            // don't toggleback if Wheel clicked
            if (!wheelClicked) {
                showingTop = !showingTop;
                updateMovePadsToMatchLpp();
                updateMoveViewPulse();
            }
            wheelClicked = false;
            return;
        }

        let lppNote = activeMoveToLppPadMap.get(moveNoteNumber);

        if (!lppNote) {
            console.log(`Move: unmapped note [${moveNoteNumber}]`);
            return;
        }

        let moveVelocity = data[2] * 2;

        // moveVelocity = moveVelocity * 2;
        if (moveVelocity > 127) {
            moveVelocity = 127;
        }

        let lppVelocity = moveVelocity; //moveVelocityToLppVelocityMap.get(data[2]) ?? moveVelocity;

        console.log(`Sending Move note ${moveNoteNumber} velocity: ${moveVelocity} to LPP pad ${lppNote} velocity: ${moveVelocity}`);
        move_midi_external_send([2 << 4 | (data[0] / 0xF), data[0], lppNote, moveVelocity]);
        return;
    }

    if (isCC) {

        console.log("control message");
        let moveControlNumber = data[1];

        let activeMoveControlToLppNoteMap = showingTop ? moveControlToLppNoteMapTop : moveControlToLppNoteMapBottom;

        let lppNote = activeMoveControlToLppNoteMap.get(moveControlNumber);

        // store current VIEW
        if (moveControlNumber === moveBACK || moveControlNumber === moveMENU || moveControlNumber === moveCAP) {
            currentView = moveControlNumber;
            updateMoveViewPulse();
        }

        // if Shift is held, exit if Wheel is pressed
        if (shiftHeld == true && moveControlNumber === moveWHEEL) {
            console.log("Shift+Wheel - exit");
            exit();
            return;
        }

        let toggleTopBottom = moveControlNumber === moveWHEEL && data[2] === 0x7f;
        if (toggleTopBottom) {
            wheelClicked = true;
//            showingTop = !showingTop;
//            updateMovePadsToMatchLpp();
            return;
        }


        if (!lppNote) {
            console.log(`Move: unmapped control [${moveControlNumber}]`);
            handleMoveKnobs(data);
            return;
        }

        let pressed = data[2] === 127;

        console.log(`Sending Move control ${moveControlNumber} to LPP pad ${lppNote} pressed:${pressed}`);
        if (pressed) {
            // detect and store Shift button state
            if (moveControlNumber === 49) {
                shiftHeld = true;
                console.log("Shift Held");
            }

            move_midi_external_send([2 << 4 | 0x9, 0x90, lppNote, 100]);

        } else {
            // reset Shift state when released
            if (moveControlNumber === 49) {
                console.log("Shift Released");
                shiftHeld = false;
            }

            move_midi_external_send([2 << 4 | 0x8, 0x80, lppNote, 0]);
        }
        return;
    }

    // if (aftertouchToModwheel(data)) {
    //     return;
    // }

    console.log(`Unmapped Move message: ${data}`);

}

// let lppInitSysex = [0xF0, 126, 0, 6, 2, 0, 32, 41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF7];

function initLPP() {
    let out_cable = 2;
    let LPPInitSysex = [
        out_cable << 4 | 0x4, 0xF0, 126, 0,
        out_cable << 4 | 0x4, 6, 2, 0,
        out_cable << 4 | 0x4, 32, 41, 0x00,
        out_cable << 4 | 0x4, 0x00, 0x00, 0x00,
        out_cable << 4 | 0x4, 0x00, 0x00, 0x00,
        out_cable << 4 | 0x6, 0x00, 0xF7, 0x0];


    // move_midi_external_send(lppInitSysex);

    // Trigger LPP mode on the M8
    console.log("Sending M8 LPP init");
    move_midi_external_send(LPPInitSysex);
    showingTop = true;

    clear_screen();
    print(0, 0, "Move Anything", 1);
    print(0, 18, "M8 connected", 1);
}

function updatePLAYLed() {
    if (liveMode === false && isPlaying === false) { move_midi_internal_send([0 << 4 | 0xb, 0xB0, movePLAY, light_grey]); };
    if (liveMode === false && isPlaying === true) { move_midi_internal_send([0 << 4 | 0xb, 0xB0, movePLAY, green]); };
    if (liveMode === true && isPlaying === false) { move_midi_internal_send([0 << 4 | 0xb, 0xB0, movePLAY, sky]); };
    if (liveMode === true && isPlaying === true) { move_midi_internal_send([0 << 4 | 0xb, 0xB0, movePLAY, navy]); };
}

globalThis.init = function () {
    console.log("Move control surface script staring...");


    clear_screen();
    print(0, 0, "Move Anything", 1);
    print(0, 18, "Waiting for", 1);
    print(0, 36, "M8 to connect", 1);
}

// globalThis.tick = function () {
// };
