/*
 * Input filtering utilities for Move hardware
 */

/* Capacitive touch events come as notes 0-9 from knob touches */
export function isCapacitiveTouch(noteNumber) {
    return noteNumber < 10;
}

/* Move pad notes range from 68-99 */
export const PAD_NOTE_MIN = 68;
export const PAD_NOTE_MAX = 99;

export function isPadNote(noteNumber) {
    return noteNumber >= PAD_NOTE_MIN && noteNumber <= PAD_NOTE_MAX;
}

/* Filter MIDI message - returns true if should be ignored */
export function shouldFilterMidi(data) {
    const status = data[0] & 0xF0;
    const isNote = status === 0x90 || status === 0x80;

    if (isNote && isCapacitiveTouch(data[1])) {
        return true;
    }
    return false;
}

/* Common CC constants */
export const CC_JOG_WHEEL = 14;
export const CC_JOG_CLICK = 3;
export const CC_SHIFT = 49;
export const CC_MENU = 50;
export const CC_LEFT = 62;
export const CC_RIGHT = 63;
export const CC_UP = 55;
export const CC_DOWN = 54;

/* Jog wheel press as note */
export const NOTE_JOG_PRESS = 9;
