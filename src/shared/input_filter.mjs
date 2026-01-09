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

/* Global flag to enable/disable LED updates (for performance testing) */
let g_leds_enabled = true;

/* LED state cache - only send when color changes */
let g_led_cache = new Array(128).fill(-1);      /* Notes (pads, steps) */
let g_button_cache = new Array(128).fill(-1);   /* CCs (buttons, knobs) */

export function setLedsEnabled(enabled) {
    g_leds_enabled = enabled;
}

export function getLedsEnabled() {
    return g_leds_enabled;
}

/* Clear LED cache - call when changing views to force full refresh */
export function clearLEDCache() {
    g_led_cache.fill(-1);
    g_button_cache.fill(-1);
}

/* Set LED color for a note (pad, step, etc.) */
export function setLED(note, color) {
    if (!g_leds_enabled) return;
    if (g_led_cache[note] === color) return;  /* No change, skip */
    g_led_cache[note] = color;
    move_midi_internal_send([0x09, MidiNoteOn, note, color]);
}

/* Set LED color via CC (for buttons) */
export function setButtonLED(cc, color) {
    if (!g_leds_enabled) return;
    if (g_button_cache[cc] === color) return;  /* No change, skip */
    g_button_cache[cc] = color;
    move_midi_internal_send([0x0b, MidiCC, cc, color]);
}

/* Clear all LEDs */
export function clearAllLEDs() {
    clearLEDCache();  /* Reset cache so all sends go through */
    for (let i = 0; i < 127; i++) {
        setLED(i, 0);
        setButtonLED(i, 0);
    }
}
