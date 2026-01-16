/*
 * SEQOMD Local copy of input filter utilities
 * (Bundled for module independence from host framework)
 */

import {
    MidiNoteOn, MidiNoteOff, MidiCC,
    MidiClock, MidiSysexStart, MidiSysexEnd,
    MidiChAftertouch, MidiPolyAftertouch,
    MovePads
} from './shared-constants.js';

/* ============ Capacitive Touch Filtering ============ */

export function isCapacitiveTouch(noteNumber) {
    return noteNumber < 10;
}

export function isCapacitiveTouchMessage(data) {
    const status = data[0] & 0xF0;
    const isNote = status === MidiNoteOn || status === MidiNoteOff;
    return isNote && isCapacitiveTouch(data[1]);
}

/* ============ MIDI Message Filtering ============ */

export function isNoiseMessage(data) {
    const status = data[0];
    return (
        status === MidiClock ||
        status === MidiSysexStart ||
        status === MidiSysexEnd ||
        status === MidiChAftertouch ||
        status === MidiPolyAftertouch
    );
}

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

let g_leds_enabled = true;
let g_led_cache = new Array(128).fill(-1);
let g_button_cache = new Array(128).fill(-1);

export function setLedsEnabled(enabled) {
    g_leds_enabled = enabled;
}

export function getLedsEnabled() {
    return g_leds_enabled;
}

export function clearLEDCache() {
    g_led_cache.fill(-1);
    g_button_cache.fill(-1);
}

export function setLED(note, color) {
    if (!g_leds_enabled) return;
    if (g_led_cache[note] === color) return;
    g_led_cache[note] = color;
    move_midi_internal_send([0x09, MidiNoteOn, note, color]);
}

export function setButtonLED(cc, color) {
    if (!g_leds_enabled) return;
    if (g_button_cache[cc] === color) return;
    g_button_cache[cc] = color;
    move_midi_internal_send([0x0b, MidiCC, cc, color]);
}

export function clearAllLEDs() {
    clearLEDCache();
    for (let i = 0; i < 127; i++) {
        setLED(i, 0);
        setButtonLED(i, 0);
    }
}
