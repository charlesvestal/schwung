/*
 * Step Sequencer UI
 * Based on controller module pattern
 */

import {
    Black, White, LightGrey, Navy, BrightGreen,
    MidiNoteOn, MidiNoteOff, MidiCC,
    MovePlay, MoveSteps, MovePads
} from "../../shared/constants.mjs";

import { 
    isNoiseMessage, isCapacitiveTouchMessage,
    setLED, setButtonLED, clearAllLEDs 
} from "../../shared/input_filter.mjs";

/* State */
let playing = false;
let heldStep = -1;
let padWasPressed = false;
let steps = new Array(16).fill(0);  // 0=off, >0=midi note

/* Display state */
let line1 = "Step Sequencer";
let line2 = "Tap steps to toggle";
let line3 = "Hold + pad = note";
let line4 = "";

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

function updateStepLEDs() {
    for (let i = 0; i < 16; i++) {
        let color = Black;
        if (steps[i] > 0) color = Navy;
        if (i === heldStep) color = White;
        setLED(MoveSteps[i], color);
    }
}

function noteToName(n) {
    if (n <= 0) return "---";
    const names = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"];
    return names[n % 12] + (Math.floor(n/12) - 1);
}

globalThis.init = function() {
    console.log("Sequencer starting...");
    clearAllLEDs();
    updateStepLEDs();
    setButtonLED(MovePlay, Black);
};

globalThis.tick = function() {
    drawUI();
};

globalThis.onMidiMessageInternal = function(data) {
    if (isNoiseMessage(data)) return;
    if (isCapacitiveTouchMessage(data)) return;
    
    const isNote = data[0] === MidiNoteOn || data[0] === MidiNoteOff;
    const isNoteOn = data[0] === MidiNoteOn;
    const isCC = data[0] === MidiCC;
    const note = data[1];
    const velocity = data[2];
    
    /* Step buttons (notes 16-31) */
    if (isNote && MoveSteps.includes(note)) {
        const idx = MoveSteps.indexOf(note);
        
        if (isNoteOn && velocity > 0) {
            /* Step pressed - start hold */
            heldStep = idx;
            padWasPressed = false;
            setLED(note, White);
            displayMessage(undefined, "Step " + (idx+1), "Hold + pad = note", undefined);
        } else {
            /* Step released */
            if (heldStep === idx && !padWasPressed) {
                /* Toggle step */
                if (steps[idx] > 0) {
                    steps[idx] = 0;
                    displayMessage(undefined, "Step " + (idx+1) + " OFF", "", undefined);
                } else {
                    steps[idx] = 60;
                    displayMessage(undefined, "Step " + (idx+1) + " ON", "Note: C4", undefined);
                }
                host_module_set_param("step_" + idx + "_note", String(steps[idx]));
            }
            heldStep = -1;
            setLED(note, steps[idx] > 0 ? Navy : Black);
        }
        return;
    }
    
    /* Pads (notes 68-99) */
    if (isNote && MovePads.includes(note)) {
        if (isNoteOn && velocity > 0 && heldStep >= 0) {
            /* Set note for held step */
            const midiNote = 36 + MovePads.indexOf(note);
            steps[heldStep] = midiNote;
            padWasPressed = true;
            host_module_set_param("step_" + heldStep + "_note", String(midiNote));
            setLED(note, BrightGreen);
            displayMessage(undefined, "Step " + (heldStep+1), "Note: " + noteToName(midiNote), undefined);
        } else if (!isNoteOn || velocity === 0) {
            /* Pad released */
            setLED(note, Black);
        }
        return;
    }
    
    /* Play button (CC 85) */
    if (isCC && note === MovePlay) {
        if (velocity > 0) {
            playing = !playing;
            host_module_set_param("playing", playing ? "1" : "0");
            setButtonLED(MovePlay, playing ? BrightGreen : Black);
            displayMessage(undefined, playing ? "PLAYING" : "STOPPED", "", undefined);
        }
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {};
