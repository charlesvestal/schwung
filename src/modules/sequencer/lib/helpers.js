/*
 * SEQOMD Helpers
 * Shared utility functions used by views and router
 */

import {
    Black, White, LightGrey, BrightGreen, BrightRed, Cyan, Purple,
    MovePlay, MoveLoop, MoveRec, MoveMenu, MoveCapture, MoveTracks
} from "../../../shared/constants.mjs";

import { setButtonLED } from "../../../shared/input_filter.mjs";

import { NUM_STEPS, TRACK_COLORS, TRACK_COLORS_DIM } from './constants.js';
import { state } from './state.js';

/* ============ DSP Communication ============ */

/**
 * Set a parameter on the DSP plugin
 */
export function setParam(key, value) {
    host_module_set_param(key, value);
}

/**
 * Get a parameter from the DSP plugin
 */
export function getParam(key) {
    return host_module_get_param(key);
}

/* ============ Note Helpers ============ */

/**
 * Convert MIDI note number to note name
 */
export function noteToName(n) {
    if (n <= 0) return "---";
    const names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    return names[n % 12] + (Math.floor(n / 12) - 1);
}

/**
 * Format array of notes as string
 */
export function notesToString(notes) {
    if (!notes || notes.length === 0) return "---";
    return notes.map(n => noteToName(n)).join(" ");
}

/* ============ CC Helpers ============ */

/**
 * Send CC to external MIDI (via DSP)
 */
export function sendCCExternal(cc, value, channel) {
    setParam(`send_cc_${channel}_${cc}`, String(value));
}

/**
 * Update CC value based on encoder movement and send it
 */
export function updateAndSendCC(ccValues, index, velocity, cc, channel) {
    let val = ccValues[index];
    if (velocity >= 1 && velocity <= 63) {
        val = Math.min(val + 1, 127);
    } else if (velocity >= 65 && velocity <= 127) {
        val = Math.max(val - 1, 0);
    }
    ccValues[index] = val;
    sendCCExternal(cc, val, channel);
    return val;
}

/* ============ Track Helpers ============ */

/**
 * Get current pattern for a track
 */
export function getCurrentPattern(trackIdx) {
    return state.tracks[trackIdx].patterns[state.tracks[trackIdx].currentPattern];
}

/**
 * Select a track
 */
export function selectTrack(trackIdx) {
    if (trackIdx >= 0 && trackIdx < state.tracks.length) {
        state.currentTrack = trackIdx;
    }
}

/**
 * Toggle track mute
 */
export function toggleTrackMute(trackIdx) {
    if (trackIdx >= 0 && trackIdx < state.tracks.length) {
        state.tracks[trackIdx].muted = !state.tracks[trackIdx].muted;
        setParam(`track_${trackIdx}_mute`, state.tracks[trackIdx].muted ? "1" : "0");
    }
}

/* ============ Transport LED Updates ============ */

/**
 * Update transport button LEDs (Play, Loop, Record)
 */
export function updateTransportLEDs() {
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);

    if (state.view === 'master') {
        setButtonLED(MoveLoop, state.sendClock ? Cyan : LightGrey);
    } else if (state.view === 'track' && state.trackMode === 'loop') {
        setButtonLED(MoveLoop, Cyan);
    } else {
        const pattern = getCurrentPattern(state.currentTrack);
        const hasCustomLoop = pattern.loopStart > 0 || pattern.loopEnd < NUM_STEPS - 1;
        setButtonLED(MoveLoop, hasCustomLoop ? TRACK_COLORS[state.currentTrack] : Black);
    }

    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
}

/**
 * Update menu button LED
 */
export function updateMenuLED() {
    setButtonLED(MoveMenu, state.view === 'master' ? 127 : 0);
}

/**
 * Update capture button LED
 */
export function updateCaptureLED() {
    const isSparkMode = state.view === 'track' && state.trackMode === 'spark';
    if (isSparkMode || state.captureHeld) {
        setButtonLED(MoveCapture, Purple);
    } else if (state.view === 'track') {
        setButtonLED(MoveCapture, 107);  // Dim purple
    } else {
        setButtonLED(MoveCapture, Black);
    }
}

/* ============ Track Button LEDs ============ */

/**
 * Update track button LEDs
 */
export function updateTrackLEDs() {
    if (state.view === 'pattern') {
        for (let i = 0; i < 4; i++) {
            const btnTrackOffset = 3 - i;
            const trackIdx = state.shiftHeld ? btnTrackOffset + 4 : btnTrackOffset;
            setButtonLED(MoveTracks[i], TRACK_COLORS[trackIdx]);
        }
        return;
    }

    for (let i = 0; i < 4; i++) {
        const btnTrackOffset = 3 - i;
        const trackIdx = state.shiftHeld ? btnTrackOffset + 4 : btnTrackOffset;
        let color = Black;

        if (trackIdx === state.currentTrack) {
            color = TRACK_COLORS[trackIdx];
        } else if (getCurrentPattern(trackIdx).steps.some(s => s.notes.length > 0 || s.cc1 >= 0 || s.cc2 >= 0)) {
            color = TRACK_COLORS_DIM[trackIdx];
        }

        if (state.tracks[trackIdx].muted) {
            color = BrightRed;
        }

        setButtonLED(MoveTracks[i], color);
    }
}
