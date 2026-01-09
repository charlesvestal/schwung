/*
 * Pattern View
 * Select patterns for each track
 * 8 columns (tracks) x 4 rows (patterns), scrollable
 */

import {
    Black, White, Cyan, BrightGreen, BrightRed, LightGrey,
    MoveSteps, MovePads, MoveTracks, MoveMainKnob, MoveShift,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack
} from "../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../shared/input_filter.mjs";

import {
    NUM_TRACKS, NUM_STEPS, NUM_PATTERNS, MASTER_CC_CHANNEL,
    MoveKnobLEDs, TRACK_COLORS, TRACK_COLORS_DIM
} from '../lib/constants.js';
import { state, displayMessage, enterSetView } from '../lib/state.js';
import * as setView from './set.js';
import { setParam, updateAndSendCC } from '../lib/helpers.js';
import { saveCurrentSet, saveAllSetsToDisk } from '../lib/persistence.js';

/* ============ View Interface ============ */

/**
 * Called when entering pattern view
 */
export function onEnter() {
    updateDisplayContent();
}

/**
 * Called when exiting pattern view
 */
export function onExit() {
    // Nothing special to clean up
}

/**
 * Handle MIDI input for pattern view
 * Returns true if handled, false to let router handle
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Step buttons - shift + step 1 enters set view */
    if (isNote && note >= 16 && note <= 31) {
        const stepIdx = note - 16;

        if (state.shiftHeld && stepIdx === 0 && isNoteOn && velocity > 0) {
            /* Save current set before going to set view */
            if (state.currentSet >= 0) {
                saveCurrentSet();
                saveAllSetsToDisk();
            }
            /* Transition to set view */
            onExit();
            enterSetView();
            setView.onEnter();
            setView.updateLEDs();
            return true;
        }
        return true;
    }

    /* Pads - select pattern for track */
    if (isNote && note >= 68 && note <= 99) {
        const padIdx = note - 68;

        if (isNoteOn && velocity > 0) {
            const trackIdx = padIdx % 8;
            const rowIdx = 3 - Math.floor(padIdx / 8);  // 0-3 from bottom
            const patternIdx = state.patternViewOffset + rowIdx;

            /* Only select if pattern is in valid range */
            if (patternIdx < NUM_PATTERNS) {
                state.tracks[trackIdx].currentPattern = patternIdx;
                setParam(`track_${trackIdx}_pattern`, String(patternIdx));

                displayMessage(
                    `PATTERNS      ${state.bpm} BPM`,
                    `Track ${trackIdx + 1} -> Pat ${patternIdx + 1}`,
                    "",
                    ""
                );
                updatePadLEDs();
            }
        }
        return true;
    }

    /* Jog wheel - scroll pattern view */
    if (isCC && note === MoveMainKnob) {
        if (velocity >= 1 && velocity <= 63) {
            state.patternViewOffset = Math.min(state.patternViewOffset + 1, NUM_PATTERNS - 4);
        } else if (velocity >= 65 && velocity <= 127) {
            state.patternViewOffset = Math.max(state.patternViewOffset - 1, 0);
        }
        updateDisplayContent();
        updatePadLEDs();
        return true;
    }

    /* Knobs - send CCs 1-8 on master channel */
    const knobCCs = [71, 72, 73, 74, 75, 76, 77, 78];
    if (isCC && knobCCs.includes(note)) {
        const knobIdx = knobCCs.indexOf(note);
        const cc = knobIdx + 1;  // CC 1-8
        const val = updateAndSendCC(state.patternCCValues, knobIdx, velocity, cc, MASTER_CC_CHANNEL);
        displayMessage(
            "PATTERNS",
            `Knob ${knobIdx + 1}: CC ${cc}`,
            `Value: ${val}`,
            ""
        );
        return true;
    }

    /* Track buttons - select track */
    if (isCC && MoveTracks.includes(note)) {
        if (velocity > 0) {
            const btnIdx = MoveTracks.indexOf(note);
            const trackBtnIdx = 3 - btnIdx;
            const trackIdx = state.shiftHeld ? trackBtnIdx + 4 : trackBtnIdx;
            state.currentTrack = trackIdx;
            updateDisplayContent();
            updateLEDs();
        }
        return true;
    }

    return false;  // Let router handle
}

/**
 * Update all LEDs for pattern view
 */
export function updateLEDs() {
    updateStepLEDs();
    updatePadLEDs();
    updateKnobLEDs();
    updateTrackButtonLEDs();
    updateTransportLEDs();
    updateCaptureLED();
    updateBackLED();
}

/**
 * Update display content for pattern view
 */
export function updateDisplayContent() {
    const patStr = state.tracks.map(t => String(t.currentPattern + 1)).join(" ");
    const viewRange = `${state.patternViewOffset + 1}-${state.patternViewOffset + 4}`;

    displayMessage(
        `PATTERNS ${viewRange}  ${state.bpm} BPM`,
        `Track:  12345678`,
        `Pattern: ${patStr}`,
        ""
    );
}

/* ============ LED Updates ============ */

function updateStepLEDs() {
    /* Steps off in pattern view - focus is on pads */
    for (let i = 0; i < NUM_STEPS; i++) {
        setLED(MoveSteps[i], Black);
    }
}

function updatePadLEDs() {
    /* Pattern mode: 8 columns (tracks) x 4 rows (patterns)
     * Uses patternViewOffset to show different pattern rows
     * Row 4 (top, indices 0-7): offset + 3
     * Row 3 (indices 8-15): offset + 2
     * Row 2 (indices 16-23): offset + 1
     * Row 1 (bottom, indices 24-31): offset + 0
     */
    for (let i = 0; i < 32; i++) {
        const padNote = MovePads[i];
        const trackIdx = i % 8;
        const rowIdx = 3 - Math.floor(i / 8);  // 0-3 from bottom
        const patternIdx = state.patternViewOffset + rowIdx;

        /* Check if pattern is in valid range */
        if (patternIdx >= NUM_PATTERNS) {
            setLED(padNote, Black);
            continue;
        }

        const isCurrentPattern = state.tracks[trackIdx].currentPattern === patternIdx;
        const patternHasContent = state.tracks[trackIdx].patterns[patternIdx].steps.some(
            s => s.notes.length > 0 || s.cc1 >= 0 || s.cc2 >= 0
        );

        let color = Black;
        if (isCurrentPattern) {
            color = TRACK_COLORS[trackIdx];  // Bright = current
        } else if (patternHasContent) {
            color = TRACK_COLORS_DIM[trackIdx];  // Dim = has content
        }

        setLED(padNote, color);
    }
}

function updateKnobLEDs() {
    /* All 8 knobs for CC in pattern mode */
    for (let i = 0; i < 8; i++) {
        setButtonLED(MoveKnobLEDs[i], Cyan);
    }
}

function updateTrackButtonLEDs() {
    /* In pattern view, all track buttons show their track color */
    for (let i = 0; i < 4; i++) {
        const btnTrackOffset = 3 - i;
        const trackIdx = state.shiftHeld ? btnTrackOffset + 4 : btnTrackOffset;
        setButtonLED(MoveTracks[i], TRACK_COLORS[trackIdx]);
    }
}

function updateTransportLEDs() {
    /* Play button */
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);

    /* Loop button - off in pattern view */
    setButtonLED(MoveLoop, Black);

    /* Record button */
    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
}

function updateCaptureLED() {
    /* Capture off in pattern view */
    setButtonLED(MoveCapture, Black);
}

function updateBackLED() {
    /* Back button lit - press to return to track view */
    setButtonLED(MoveBack, White);
}
