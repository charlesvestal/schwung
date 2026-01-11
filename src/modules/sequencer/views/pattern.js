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

/* ============ Track Button Navigation ============ */

/**
 * Get the track index shown at a given button position.
 * Button 0 (top, CC 43): Selected track
 * Buttons 1-3: Tracks from scroll position, skipping selected track
 */
function getTrackAtButton(btnPosition) {
    if (btnPosition === 0) {
        return state.currentTrack;
    }

    /* Build list of tracks excluding selected */
    const otherTracks = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        if (t !== state.currentTrack) {
            otherTracks.push(t);
        }
    }

    /* Get track from scroll position */
    const scrollIdx = state.trackScrollPosition + (btnPosition - 1);
    if (scrollIdx >= 0 && scrollIdx < otherTracks.length) {
        return otherTracks[scrollIdx];
    }
    return -1;
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
            /* Shift + jog: scroll patterns vertically */
            if (velocity >= 1 && velocity <= 63) {
                state.patternViewOffset = Math.min(state.patternViewOffset + 1, NUM_PATTERNS - 4);
            } else if (velocity >= 65 && velocity <= 127) {
                state.patternViewOffset = Math.max(state.patternViewOffset - 1, 0);
            }
        } else {
            /* Jog alone: scroll tracks horizontally (1 at a time, showing 8 tracks) */
            const maxScroll = NUM_TRACKS - 8;  /* Can scroll 0-8 to show tracks 0-7 through 8-15 */
            if (velocity >= 1 && velocity <= 63) {
                state.trackScrollPosition = Math.min(state.trackScrollPosition + 1, maxScroll);
            } else if (velocity >= 65 && velocity <= 127) {
                state.trackScrollPosition = Math.max(state.trackScrollPosition - 1, 0);
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
    const endTrack = Math.min(state.trackScrollPosition + 8, NUM_TRACKS);
    const trackRange = `${startTrack}-${endTrack}`;
    const patStr = state.tracks.slice(state.trackScrollPosition, state.trackScrollPosition + 8)
        .map(t => String(t.currentPattern + 1)).join(" ");
    const viewRange = `P${state.patternViewOffset + 1}-${state.patternViewOffset + 4}`;

    displayMessage(
        `PATTERNS T${trackRange} ${viewRange}`,
        `Patterns: ${patStr}`,
        `Selected: Track ${state.currentTrack + 1}`,
        `${state.bpm} BPM`
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
    /* All 8 knobs for CC in pattern mode */
    for (let i = 0; i < 8; i++) {
        setButtonLED(MoveKnobLEDs[i], Cyan);
    }
}

function updateTrackButtonLEDs() {
    /* Track buttons use scroll navigation (selected at top + 3 scrollable) */
    for (let i = 0; i < 4; i++) {
        const btnPosition = 3 - i;  /* Top = 0, bottom = 3 */
        const trackIdx = getTrackAtButton(btnPosition);

        if (trackIdx < 0 || trackIdx >= NUM_TRACKS) {
            setButtonLED(MoveTracks[i], Black);
            continue;
        }

        let color = Black;

        if (trackIdx === state.currentTrack) {
            /* Selected track - always bright */
            color = TRACK_COLORS[trackIdx];
        } else if (getCurrentPattern(trackIdx).steps.some(s => s.notes.length > 0 || s.cc1 >= 0 || s.cc2 >= 0)) {
            /* Has content - dim color */
            color = TRACK_COLORS_DIM[trackIdx];
        } else {
            /* Empty track - dark grey */
            color = DarkGrey;
        }

        setButtonLED(MoveTracks[i], color);
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
