/*
 * Pattern View
 * Select patterns for each track
 * 8 columns (tracks) x 4 rows (patterns), scrollable
 */

import {
    Black, White, Cyan, BrightGreen, BrightRed, LightGrey, DarkGrey,
    MoveSteps, MovePads, MoveTracks, MoveMainKnob,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack,
    MoveStep1UI, MoveStep2UI, MoveStep5UI, MoveStep7UI
} from "../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../shared/input_filter.mjs";

import {
    NUM_TRACKS, NUM_STEPS, NUM_PATTERNS, MASTER_CC_CHANNEL,
    MoveKnobLEDs, TRACK_COLORS, TRACK_COLORS_DIM
} from '../lib/constants.js';
import { state, displayMessage } from '../lib/state.js';
import { setParam, updateAndSendCC, getCurrentPattern } from '../lib/helpers.js';
import { markDirty } from '../lib/persistence.js';

/* ============ Pattern Snapshots ============ */

/**
 * Save current pattern selections to a snapshot slot
 */
function savePatternSnapshot(stepIdx) {
    const snapshot = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        snapshot.push(state.tracks[t].currentPattern);
    }
    /* Ensure array is long enough */
    while (state.patternSnapshots.length <= stepIdx) {
        state.patternSnapshots.push(null);
    }
    state.patternSnapshots[stepIdx] = snapshot;
    markDirty();
}

/**
 * Recall patterns from a snapshot slot
 */
function recallPatternSnapshot(stepIdx) {
    if (!state.patternSnapshots[stepIdx]) return false;
    const snapshot = state.patternSnapshots[stepIdx];
    for (let t = 0; t < NUM_TRACKS && t < snapshot.length; t++) {
        state.tracks[t].currentPattern = snapshot[t];
        setParam(`track_${t}_pattern`, String(snapshot[t]));
    }
    state.activePatternSnapshot = stepIdx;  /* Track which snapshot is active */
    markDirty();
    return true;
}

/* ============ Track Button Navigation ============ */

/**
 * Get the track index shown at a given button position.
 * All 4 buttons show consecutive tracks from scroll position (0-3 or 8-11)
 */
function getTrackAtButton(btnPosition) {
    /* Show 4 tracks from current scroll position (0-3 or 8-11) */
    const trackIdx = state.trackScrollPosition + btnPosition;
    if (trackIdx >= 0 && trackIdx < NUM_TRACKS) {
        return trackIdx;
    }
    return -1;  // Out of range
}

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
    state.captureHeld = false;
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

    /* Pads - select pattern for track */
    if (isNote && note >= 68 && note <= 99) {
        const padIdx = note - 68;

        if (isNoteOn && velocity > 0) {
            /* Track offset based on scroll position (shows 8 tracks at a time) */
            const trackIdx = (padIdx % 8) + state.trackScrollPosition;
            const rowIdx = 3 - Math.floor(padIdx / 8);  // 0-3 from bottom
            const patternIdx = state.patternViewOffset + rowIdx;

            /* Only select if track and pattern are in valid range */
            if (trackIdx < NUM_TRACKS && patternIdx < NUM_PATTERNS) {
                state.tracks[trackIdx].currentPattern = patternIdx;
                setParam(`track_${trackIdx}_pattern`, String(patternIdx));
                state.activePatternSnapshot = -1;  /* Manual change invalidates active snapshot */
                markDirty();

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

    /* Jog wheel - scroll tracks horizontally (1 at a time) */
    if (isCC && note === MoveMainKnob) {
        if (state.shiftHeld) {
            /* Shift + jog: scroll patterns vertically (4 at a time) */
            if (velocity >= 1 && velocity <= 63) {
                /* Clockwise: increment by 4, cap at 12 */
                state.patternViewOffset = Math.min(state.patternViewOffset + 4, 12);
            } else if (velocity >= 65 && velocity <= 127) {
                /* Counter-clockwise: decrement by 4, floor at 0 */
                state.patternViewOffset = Math.max(state.patternViewOffset - 4, 0);
            }
        } else {
            /* Jog alone: scroll tracks horizontally (toggle between tracks 1-8 and 9-16) */
            if (velocity >= 1 && velocity <= 63) {
                /* Clockwise: jump to tracks 9-16 */
                state.trackScrollPosition = 8;
            } else if (velocity >= 65 && velocity <= 127) {
                /* Counter-clockwise: jump to tracks 1-8 */
                state.trackScrollPosition = 0;
            }
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

    /* Track buttons - select track (scroll navigation) */
    if (isCC && MoveTracks.includes(note)) {
        if (velocity > 0) {
            const btnIdx = MoveTracks.indexOf(note);
            const btnPosition = 3 - btnIdx;  /* Top = 0, bottom = 3 */
            const trackIdx = getTrackAtButton(btnPosition);
            if (trackIdx >= 0 && trackIdx < NUM_TRACKS) {
                state.currentTrack = trackIdx;
                /* Reset scroll position when selecting new track */
                state.trackScrollPosition = 0;
                updateDisplayContent();
                updateLEDs();
            }
        }
        return true;
    }

    /* Capture button - track hold state for snapshot save mode */
    if (isCC && note === MoveCapture) {
        state.captureHeld = velocity > 0;
        if (state.captureHeld) {
            displayMessage(
                "SNAPSHOT SAVE MODE",
                "Press a step to save",
                "current pattern selection",
                ""
            );
        } else {
            updateDisplayContent();  /* Return to normal display */
        }
        updateLEDs();
        return true;
    }

    /* Steps - save/recall pattern snapshots */
    if (isNote && note >= 16 && note <= 31) {
        const stepIdx = note - 16;
        if (isNoteOn && velocity > 0) {
            if (state.captureHeld) {
                /* Capture + Step: save current patterns to this slot */
                savePatternSnapshot(stepIdx);
                displayMessage(
                    `PATTERNS      ${state.bpm} BPM`,
                    `Saved to Snapshot ${stepIdx + 1}`,
                    "",
                    ""
                );
            } else {
                /* Step alone: recall patterns from this slot */
                if (recallPatternSnapshot(stepIdx)) {
                    const patStr = state.tracks.slice(state.trackScrollPosition, state.trackScrollPosition + 8)
                        .map(t => String(t.currentPattern + 1)).join(" ");
                    displayMessage(
                        `PATTERNS      ${state.bpm} BPM`,
                        `Recalled Snapshot ${stepIdx + 1}`,
                        `Patterns: ${patStr}`,
                        ""
                    );
                    updatePadLEDs();
                }
            }
            updateStepLEDs();
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
    updateStepUILEDs();
    updatePadLEDs();
    updateKnobLEDs();
    updateTrackButtonLEDs();
    updateTransportLEDs();
    updateCaptureLED();
    updateBackLED();
}

function updateStepUILEDs() {
    /* Clear step UI mode icons - not used in pattern view */
    setButtonLED(MoveStep1UI, Black);
    setButtonLED(MoveStep2UI, Black);
    setButtonLED(MoveStep5UI, Black);
    setButtonLED(MoveStep7UI, Black);
}

/**
 * Update display content for pattern view
 */
export function updateDisplayContent() {
    const startTrack = state.trackScrollPosition + 1;
    const endTrack = state.trackScrollPosition + 8;
    const trackRange = `${startTrack}-${endTrack}`;
    const patStr = state.tracks.slice(state.trackScrollPosition, state.trackScrollPosition + 8)
        .map(t => String(t.currentPattern + 1)).join(" ");
    const startPattern = state.patternViewOffset + 1;
    const endPattern = state.patternViewOffset + 4;
    const viewRange = `P${startPattern}-${endPattern}`;

    displayMessage(
        `PATTERNS T${trackRange} ${viewRange}`,
        `Patterns: ${patStr}`,
        `Selected: Track ${state.currentTrack + 1}`,
        `${state.bpm} BPM`
    );
}

/* ============ LED Updates ============ */

function updateStepLEDs() {
    /* Steps show saved pattern snapshots */
    for (let i = 0; i < NUM_STEPS; i++) {
        const hasSnapshot = state.patternSnapshots[i] !== null && state.patternSnapshots[i] !== undefined;
        const isActive = state.activePatternSnapshot === i;

        if (state.captureHeld) {
            /* Capture held: show all steps lit to indicate save mode */
            setLED(MoveSteps[i], hasSnapshot ? BrightRed : White);
        } else {
            /* Normal mode: show saved snapshots, highlight active one */
            if (isActive) {
                setLED(MoveSteps[i], BrightGreen);  /* Active snapshot = bright green */
            } else if (hasSnapshot) {
                setLED(MoveSteps[i], Cyan);  /* Saved snapshot = cyan */
            } else {
                setLED(MoveSteps[i], Black);  /* Empty = black */
            }
        }
    }
}

function updatePadLEDs() {
    /* Pattern mode: 8 columns (tracks) x 4 rows (patterns)
     * Uses patternViewOffset to show different pattern rows
     * Uses trackScrollPosition for horizontal scrolling (shows 8 tracks at a time)
     * Row 4 (top, indices 0-7): offset + 3
     * Row 3 (indices 8-15): offset + 2
     * Row 2 (indices 16-23): offset + 1
     * Row 1 (bottom, indices 24-31): offset + 0
     */
    for (let i = 0; i < 32; i++) {
        const padNote = MovePads[i];
        const trackIdx = (i % 8) + state.trackScrollPosition;
        const rowIdx = 3 - Math.floor(i / 8);  // 0-3 from bottom
        const patternIdx = state.patternViewOffset + rowIdx;

        /* Check if track and pattern are in valid range */
        if (trackIdx >= NUM_TRACKS || patternIdx >= NUM_PATTERNS) {
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
    /* Knobs match the track colors below them */
    for (let i = 0; i < 8; i++) {
        const trackIdx = state.trackScrollPosition + i;
        if (trackIdx < NUM_TRACKS) {
            setButtonLED(MoveKnobLEDs[i], TRACK_COLORS[trackIdx]);
        } else {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
    }
}

function updateTrackButtonLEDs() {
    /* Track buttons show 4 tracks from scroll position, selected is brighter */
    for (let i = 0; i < 4; i++) {
        const btnPosition = 3 - i;  /* Top = 0, bottom = 3 */
        const trackIdx = getTrackAtButton(btnPosition);

        if (trackIdx < 0 || trackIdx >= NUM_TRACKS) {
            setButtonLED(MoveTracks[i], Black);
            continue;
        }

        let color = Black;

        if (trackIdx === state.currentTrack) {
            /* Selected track - bright */
            color = TRACK_COLORS[trackIdx];
        } else {
            /* Non-selected track - dim color (shows track identity even when empty) */
            color = TRACK_COLORS_DIM[trackIdx];
        }

        setButtonLED(MoveTracks[i], color);
    }
}

function updateTransportLEDs() {
    /* Play button */
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);

    /* Loop button - off in pattern view */
    setButtonLED(MoveLoop, Black);

    /* Rec button - shows recording state */
    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
}

function updateCaptureLED() {
    /* Capture button - lit when held for snapshot save mode */
    setButtonLED(MoveCapture, state.captureHeld ? White : Black);
}

function updateBackLED() {
    /* Back button lit - press to return to track view */
    setButtonLED(MoveBack, White);
}
