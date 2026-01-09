/*
 * Set View
 * Select which set (song) to work on
 * 32 sets displayed on pads, current set highlighted
 */

import {
    Black, White, Cyan, BrightGreen, BrightRed,
    MoveSteps, MovePads, MoveTracks, MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack
} from "../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../shared/input_filter.mjs";

import { NUM_SETS, NUM_STEPS, TRACK_COLORS } from '../lib/constants.js';
import { state, displayMessage, enterTrackView } from '../lib/state.js';
import * as trackView from './track.js';
import { setParam, syncAllTracksToDSP } from '../lib/helpers.js';
import { saveCurrentSet, saveAllSetsToDisk, loadSetToTracks, setHasContent } from '../lib/persistence.js';

/* ============ View Interface ============ */

/**
 * Called when entering set view
 */
export function onEnter() {
    updateDisplayContent();
}

/**
 * Called when exiting set view
 */
export function onExit() {
    // Nothing special to clean up
}

/**
 * Handle MIDI input for set view
 * Returns true if handled, false to let router handle
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Pads - select a set */
    if (isNote && note >= 68 && note <= 99) {
        const padIdx = note - 68;

        if (isNoteOn && velocity > 0) {
            /* Convert pad index to set index (bottom-left = set 0) */
            const row = 3 - Math.floor(padIdx / 8);  // Row 0 is bottom
            const col = padIdx % 8;
            const setIdx = row * 8 + col;

            /* Save current set if we have one loaded */
            if (state.currentSet >= 0) {
                saveCurrentSet();
                saveAllSetsToDisk();
            }

            /* Load the selected set */
            loadSetToTracks(setIdx);

            /* Sync all track data to DSP */
            syncAllTracksToDSP();

            /* Transition to track view */
            onExit();
            enterTrackView();
            trackView.onEnter();
            trackView.updateLEDs();

            displayMessage(
                `SEQOMD - Set ${setIdx + 1}`,
                `Track ${state.currentTrack + 1}`,
                "",
                ""
            );

            return true;
        }
        return true;
    }

    /* Track buttons - inactive in set view, consume but ignore */
    if (isCC && MoveTracks.includes(note)) {
        return true;
    }

    return false;  // Let router handle
}

/**
 * Update all LEDs for set view
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
 * Update display content for set view
 */
export function updateDisplayContent() {
    displayMessage(
        "SEQOMD - SELECT SET",
        state.currentSet >= 0 ? `Current: Set ${state.currentSet + 1}` : "",
        "",
        ""
    );
}

/* ============ LED Updates ============ */

function updateStepLEDs() {
    /* Steps off in set view - focus is on pads */
    for (let i = 0; i < NUM_STEPS; i++) {
        setLED(MoveSteps[i], Black);
    }
}

function updatePadLEDs() {
    /* Set view: 32 pads = 32 sets
     * Layout: 4 rows x 8 columns
     * Row 4 (top, indices 0-7): Sets 25-32
     * Row 3 (indices 8-15): Sets 17-24
     * Row 2 (indices 16-23): Sets 9-16
     * Row 1 (bottom, indices 24-31): Sets 1-8
     */
    for (let i = 0; i < 32; i++) {
        const padNote = MovePads[i];
        /* Convert pad index to set index (bottom-left = set 0) */
        const row = 3 - Math.floor(i / 8);  // Row 0 is bottom
        const col = i % 8;
        const setIdx = row * 8 + col;

        const isCurrentSet = state.currentSet === setIdx;
        const hasContent = setHasContent(setIdx);

        let color = Black;
        if (isCurrentSet) {
            color = Cyan;  // Currently loaded set
        } else if (hasContent) {
            /* Use track colors cycling through for sets with content */
            color = TRACK_COLORS[setIdx % 8];
        }
        /* Empty sets stay Black */

        setLED(padNote, color);
    }
}

function updateKnobLEDs() {
    /* Knobs off in set view */
    // Knob LEDs handled by router
}

function updateTrackButtonLEDs() {
    /* Track buttons off in set view - not active */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }
}

function updateTransportLEDs() {
    /* Play button */
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);

    /* Loop button - off in set view */
    setButtonLED(MoveLoop, Black);

    /* Record button */
    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
}

function updateCaptureLED() {
    /* Capture off in set view */
    setButtonLED(MoveCapture, Black);
}

function updateBackLED() {
    /* Back button lit - press to return to track view */
    setButtonLED(MoveBack, White);
}
