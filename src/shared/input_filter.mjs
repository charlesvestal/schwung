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

const ledCache = new Array(128).fill(-1);
const buttonCache = new Array(128).fill(-1);

/* Set LED color for a note (pad, step, etc.) */
export function setLED(note, color, force = false) {
    if (!force && ledCache[note] === color) return;
    ledCache[note] = color;
    move_midi_internal_send([0x09, MidiNoteOn, note, color]);
}

/* Set LED color via CC (for buttons) */
export function setButtonLED(cc, color, force = false) {
    if (!force && buttonCache[cc] === color) return;
    buttonCache[cc] = color;
    move_midi_internal_send([0x0b, MidiCC, cc, color]);
}

/* Clear all LEDs */
export function clearAllLEDs() {
    for (let i = 0; i < 128; i++) {
        ledCache[i] = -1;
        buttonCache[i] = -1;
    }
    for (let i = 0; i < 127; i++) {
        setLED(i, 0, true);
        setButtonLED(i, 0, true);
    }
}

/* ============ Encoder Delta Decoding ============ */

/**
 * Decode encoder/jog delta from CC value
 * @param {number} value - CC value (1-63 = clockwise, 65-127 = counter-clockwise)
 * @returns {number} Delta (-1, 0, or 1)
 */
export function decodeDelta(value) {
    if (value === 0) return 0;
    if (value >= 1 && value <= 63) return 1;
    if (value >= 65 && value <= 127) return -1;
    return 0;
}

/* ============ Encoder Acceleration ============ */

/* Acceleration settings */
const ACCEL_MIN_STEP = 1;
const ACCEL_MAX_STEP = 10;
const ACCEL_SLOW_THRESHOLD = 150;  /* ms - slower than this = min step */
const ACCEL_FAST_THRESHOLD = 25;   /* ms - faster than this = max step */

/* Track last event time per encoder */
const encoderLastTime = new Map();

/**
 * Decode encoder delta with acceleration applied
 * Slow turns = fine control (step 1), fast turns = coarse control (larger steps)
 * @param {number} value - CC value from encoder
 * @param {number} encoderId - Encoder identifier (CC number or index)
 * @returns {number} Accelerated delta (signed, preserves direction)
 */
export function decodeAcceleratedDelta(value, encoderId = 0) {
    const rawDelta = decodeDelta(value);
    if (rawDelta === 0) return 0;

    const now = Date.now();
    const lastTime = encoderLastTime.get(encoderId) || 0;
    const elapsed = now - lastTime;
    encoderLastTime.set(encoderId, now);

    /* Calculate step based on speed */
    let step;
    if (elapsed <= 0 || elapsed >= ACCEL_SLOW_THRESHOLD) {
        step = ACCEL_MIN_STEP;
    } else if (elapsed <= ACCEL_FAST_THRESHOLD) {
        step = ACCEL_MAX_STEP;
    } else {
        const speedRatio = (ACCEL_SLOW_THRESHOLD - elapsed) / (ACCEL_SLOW_THRESHOLD - ACCEL_FAST_THRESHOLD);
        step = Math.round(ACCEL_MIN_STEP + speedRatio * (ACCEL_MAX_STEP - ACCEL_MIN_STEP));
    }

    return rawDelta * step;
}

/**
 * Reset acceleration tracking for an encoder
 * @param {number} encoderId - Encoder identifier
 */
export function resetEncoderAccel(encoderId) {
    encoderLastTime.delete(encoderId);
}

/**
 * Reset all encoder acceleration tracking
 */
export function resetAllEncoderAccel() {
    encoderLastTime.clear();
}
