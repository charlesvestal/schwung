/*
 * Input filtering and hardware utilities for Move modules
 */

import {
    MidiNoteOn, MidiNoteOff, MidiCC,
    MidiClock, MidiSysexStart, MidiSysexEnd,
    MidiChAftertouch, MidiPolyAftertouch,
    MovePads
} from './constants.mjs';

/* ============ Capacitive Touch Filtering ============ */

/* Capacitive touch events come as notes 0-9 from knob touches */
export function isCapacitiveTouch(noteNumber) {
    return noteNumber < 10;
}

/* Check if a MIDI message is a capacitive touch event */
export function isCapacitiveTouchMessage(data) {
    const status = data[0] & 0xF0;
    const isNote = status === MidiNoteOn || status === MidiNoteOff;
    return isNote && isCapacitiveTouch(data[1]);
}

/* ============ MIDI Message Filtering ============ */

/* Messages to filter out (noise from hardware) */
export function isNoiseMessage(data) {
    const status = data[0];
    return (
        status === MidiClock ||           // 0xF8 - MIDI clock
        status === MidiSysexStart ||      // 0xF0 - Sysex start
        status === MidiSysexEnd ||        // 0xF7 - Sysex end
        status === MidiChAftertouch ||    // 0xD0 - Channel aftertouch
        status === MidiPolyAftertouch     // 0xA0 - Poly aftertouch
    );
}

/* Combined filter - returns true if message should be ignored */
export function shouldFilterMessage(data) {
    return isNoiseMessage(data) || isCapacitiveTouchMessage(data);
}

/* ============ Pad Utilities ============ */

export function isPadNote(noteNumber) {
    return MovePads.includes(noteNumber);
}

export function getPadIndex(noteNumber) {
    return MovePads.indexOf(noteNumber);
}

/* ============ LED Control ============ */

/* Set LED color for a note (pad, step, etc.) */
export function setLED(note, color) {
    move_midi_internal_send([0x09, MidiNoteOn, note, color]);
}

/* Set LED color via CC (for buttons) */
export function setButtonLED(cc, color) {
    move_midi_internal_send([0x0b, MidiCC, cc, color]);
}

/* Clear all LEDs */
export function clearAllLEDs() {
    for (let i = 0; i < 127; i++) {
        setLED(i, 0);
        setButtonLED(i, 0);
    }
}
