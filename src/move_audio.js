import { aftertouchToModwheel } from "./aftertouch_to_modwheel.mjs";
import { handleMoveKnobs } from "./move_virtual_knobs.mjs";
import { clamp } from "./math_helpers.mjs";

/*
Notes: 
Sequencer 16 - 31
Grid 68-99 (Bottom left to top right)
Sequencer steps: 16-31
Track selectors 40-43

CCs:
UI items below sequencer steps: 16-31
Shift 49
Menu 50
Capture 52
Down 54
Up 55
Undo 56
Loop 58
Copy 60
Left 62
Right 63
Knob Indicators 71-78
Play 85
Rec 86
Mute 88
Record (audio) 118
Delete 119
*/

globalThis.onMidiMessageExternal = function (data) {
    console.log(`onMidiMessageExternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);

    move_midi_internal_send([0 << 4 | (data[0] / 16), data[0], data[1], data[2]]);
}

let jogwheelValue = 0;

let noteDown = new Set();

globalThis.onMidiMessageInternal = function (data) {
    console.log(`onMidiMessageInternal ${data[0].toString(16)} ${data[1].toString(16)} ${data[2].toString(16)}`);

    let isNote = (data[0] & 0xF0) === 0x80 || (data[0] & 0xF0) === 0x90;

    let ignoreCapacitiveTouch = isNote && data[1] < 10;

    if (ignoreCapacitiveTouch) {
        return;
    }

    if (handleMoveKnobs(data)) {
        return;
    }

    if (aftertouchToModwheel(data)) {
        return;
    }

    move_midi_external_send([2 << 4 | (data[0] / 16), data[0], data[1], data[2]]);

    if (isNote) {
        if ((data[0] & 0xF0) === 0x90) {
            console.log(`Adding note ${data[1]}`);
            noteDown.add(data[1])
        }

        if ((data[0] & 0xF0) === 0x80) {
            console.log(`Deleting note ${data[1]}`);
            noteDown.delete(data[1]);
        }

        move_midi_internal_send([0 << 4 | (data[0] / 16), data[0], data[1], 23]);
        return;
    }

    let isCC = data[0] === 0xb0;
    if (isCC) {
        let ccNumber = data[1];
        if (ccNumber === 14) {
            jogwheelValue += (data[2] === 1 ? 1 : -1);
            console.log(jogwheelValue);
        }


        // let controlColor = data[2] === 127 ? 23 : 0;
        let controlColor = 23;
        move_midi_internal_send([0 << 4 | (data[0] / 16), data[0], data[1], controlColor]);

    }
}

/*
        https://www.usb.org/sites/default/files/midi10.pdf

        0x5     1               Single-byte System Common Message or SysEx ends with following single byte.
        0x6     2               SysEx ends with following two bytes.
        0x7     3               SysEx ends with following three bytes.
*/

// Example: [F0 00 21 1D 01 01 05 F7] = trigger palette reapplication

// Example: [F0 00 21 1D 01 01 03 7D 00 00 00 00 7F 01 7E 00 F7] = set entry 125 to 0/0/255 and 126

// CC 16-31 icons
// 40-43 track selectors
// 71 - 78 knob LEDs

let displayWidth = 128;
let displayHeight = 64;
let positionX = displayWidth / 2;
let positionY = displayHeight / 2;
let speedX = 0.5;
let speedY = 0.3;
// let failCounter = 0;

let sampleRate = 44100;
let numSamples = 1 * sampleRate;
let sampleBuffer = new Int16Array(numSamples);

let outputFrame = 0;
let pitch = 0.05;
function playNote(note) {
    let audioInOffset = 2048 + 256;
    for (let frame = 0; frame < 512 / 4; frame++) {
        let inputL = get_int16(audioInOffset + frame * 4);
        let inputR = get_int16(audioInOffset + frame * 4 + 2);

        inputL /= 32767.0;
        inputR /= 32767.0;

        // input is not being used.        
        const frequency = 440 * Math.pow(2, (note - 69) / 12);
        const step = (2 * Math.PI * frequency) / sampleRate;

        let outputL = Math.sin(outputFrame * step) / 10;
        let outputR = Math.sin(outputFrame * step) / 10;
        outputFrame++;

        // pitch += 0.000001;
        // if (pitch > 0.5) {
        //     pitch = 0.1
        // }

        // do stuff

        set_int16(256 + frame * 4 + 0, outputL * 32767 & 0xFFFF);
        set_int16(256 + frame * 4 + 2, outputR * 32767 & 0xFFFF);
    }
}

function playSilence() {
    let audioInOffset = 2048 + 256;
    for (let frame = 0; frame < 512 / 4; frame++) {
        set_int16(256 + frame * 4 + 0, 0 * 32767 & 0xFFFF);
        set_int16(256 + frame * 4 + 2, 0 * 32767 & 0xFFFF);
    }
}

// globalThis.init = function () {
// }


globalThis.tick = function (deltaTime) {

    let notes = [...noteDown];
    
    if (notes.length) {
        playNote(notes[0]);
        clear_screen();
        print(0, 0, "Move Anything", 1);
        print(0, 18, "Audio sample", 1);
        print(0, 36, "Note:" + notes[0], 1);

    } else {
        playSilence();
        clear_screen();
        print(0, 0, "Move Anything", 1);
        print(0, 18, "Audio sample", 1);
        print(0, 36, "Press a key!", 1);

    }
}