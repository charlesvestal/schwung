/*
 * Master View
 * CC output mode for controlling external gear
 * Each knob controls MIDI channel for its track
 */

import {
    Black, LightGrey, BrightGreen, Cyan, BrightRed, VividYellow,
    MoveSteps, MovePads, MoveLoop,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8
} from "../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../shared/input_filter.mjs";

import { NUM_TRACKS, NUM_STEPS, MoveKnobLEDs, TRACK_COLORS } from '../lib/constants.js';
import { state, displayMessage } from '../lib/state.js';
import { setParam, updateTransportLEDs } from '../lib/helpers.js';

/* ============ Master-specific Data ============ */

const TRACK_TYPE_DRUM = 0;
const TRACK_TYPE_NOTE = 1;
const TRACK_TYPE_ARP = 2;
const TRACK_TYPE_CHORD = 3;
const TRACK_TYPE_COLORS = [BrightRed, BrightGreen, Cyan, VividYellow];

/* Master data stored locally - could move to state if needed */
const masterData = {
    followChord: new Array(NUM_TRACKS).fill(false),
    trackType: [TRACK_TYPE_DRUM, TRACK_TYPE_DRUM, TRACK_TYPE_DRUM, TRACK_TYPE_DRUM,
                TRACK_TYPE_NOTE, TRACK_TYPE_NOTE, TRACK_TYPE_ARP, TRACK_TYPE_CHORD],
    rootNote: 0,
    scale: 0
};

/* ============ View Interface ============ */

/**
 * Called when entering master view
 */
export function onEnter() {
    updateDisplayContent();
}

/**
 * Called when exiting master view
 */
export function onExit() {
    // Nothing special to clean up
}

/**
 * Handle MIDI input for master view
 * Returns true if handled, false to let router handle
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Pads - chord follow (row 1) and track type (row 2) */
    if (isNote && note >= 68 && note <= 99) {
        const padIdx = note - 68;

        if (isNoteOn && velocity > 0) {
            if (padIdx >= 24) {
                /* Row 1 (bottom): Toggle chord follow for track */
                const trackIdx = padIdx - 24;
                masterData.followChord[trackIdx] = !masterData.followChord[trackIdx];
                setParam(`track_${trackIdx}_follow_chord`,
                    masterData.followChord[trackIdx] ? "1" : "0");
                updateDisplayContent();
                updatePadLEDs();
                return true;
            } else if (padIdx >= 16) {
                /* Row 2: Cycle track type */
                const trackIdx = padIdx - 16;
                masterData.trackType[trackIdx] = (masterData.trackType[trackIdx] + 1) % 4;
                setParam(`track_${trackIdx}_type`, String(masterData.trackType[trackIdx]));
                updateDisplayContent();
                updatePadLEDs();
                return true;
            }
        }
        return true;
    }

    /* Knobs - change MIDI channel for each track */
    const knobs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];
    if (isCC && knobs.includes(note)) {
        const knobIdx = knobs.indexOf(note);
        const trackIdx = knobIdx;
        let channel = state.tracks[trackIdx].channel;

        if (velocity >= 1 && velocity <= 63) {
            channel = (channel + 1) % 16;
        } else if (velocity >= 65 && velocity <= 127) {
            channel = (channel - 1 + 16) % 16;
        }

        state.tracks[trackIdx].channel = channel;
        setParam(`track_${trackIdx}_channel`, String(channel));
        updateDisplayContent();
        return true;
    }

    /* Loop button - toggle clock output */
    if (isCC && note === MoveLoop) {
        if (velocity > 0) {
            state.sendClock = state.sendClock ? 0 : 1;
            setParam("send_clock", String(state.sendClock));
            updateDisplayContent();
            updateTransportLEDs();
        }
        return true;
    }

    return false;  // Let router handle
}

/**
 * Update all LEDs for master view
 */
export function updateLEDs() {
    updateStepLEDs();
    updatePadLEDs();
    updateKnobLEDs();
}

/**
 * Update display content for master view
 */
export function updateDisplayContent() {
    const chStr = state.tracks.map(t => String(t.channel + 1).padStart(2)).join("");
    displayMessage(
        "Track:  12345678",
        `Ch: ${chStr}`,
        `Sync: ${state.sendClock ? "ON" : "OFF"}`,
        ""
    );
}

/* ============ LED Updates ============ */

function updateStepLEDs() {
    for (let i = 0; i < NUM_STEPS; i++) {
        setLED(MoveSteps[i], LightGrey);
    }
}

function updatePadLEDs() {
    /* Master mode pad layout:
     * Row 4 (top, indices 0-7): Reserved
     * Row 3 (indices 8-15): Reserved
     * Row 2 (indices 16-23): Track type for each track
     * Row 1 (bottom, indices 24-31): Chord follow toggle for each track
     */
    for (let i = 0; i < 32; i++) {
        const padNote = MovePads[i];
        if (i >= 24) {
            /* Row 1 (bottom): Chord follow toggles */
            const trackIdx = i - 24;
            const followColor = masterData.followChord[trackIdx] ? TRACK_COLORS[trackIdx] : LightGrey;
            setLED(padNote, followColor);
        } else if (i >= 16) {
            /* Row 2: Track types */
            const trackIdx = i - 16;
            setLED(padNote, TRACK_TYPE_COLORS[masterData.trackType[trackIdx]]);
        } else {
            /* Rows 3-4 (top): Reserved */
            setLED(padNote, Black);
        }
    }
}

function updateKnobLEDs() {
    /* Each knob shows its track color */
    for (let i = 0; i < 8; i++) {
        setButtonLED(MoveKnobLEDs[i], TRACK_COLORS[i]);
    }
}
