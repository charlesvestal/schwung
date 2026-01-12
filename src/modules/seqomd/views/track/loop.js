/*
 * Track View - Loop Mode
 * Edit loop start and end points
 *
 * This mode is fully self-contained:
 * - Owns ALL LEDs
 * - Handles step buttons for loop selection
 * - No track selection, no other controls
 */

import {
    Black, LightGrey, Cyan, BrightGreen, BrightRed,
    MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack
} from "../../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../../shared/input_filter.mjs";
import { NUM_STEPS, MoveKnobLEDs, TRACK_COLORS } from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import { setParam, getCurrentPattern } from '../../lib/helpers.js';
import { markDirty } from '../../lib/persistence.js';

/* ============ Input Handling ============ */

/**
 * Handle input in loop edit mode
 * Only responds to step buttons - all other input ignored
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const note = data[1];
    const velocity = data[2];

    /* Step buttons - set loop start/end */
    if (isNote && note >= 16 && note <= 31 && isNoteOn && velocity > 0) {
        const stepIdx = note - 16;
        handleLoopEditStep(stepIdx);
        return true;
    }

    /* Ignore all other input - mode is focused */
    return true;
}

function handleLoopEditStep(stepIdx) {
    if (state.loopEditFirst < 0) {
        /* First tap - set start point */
        state.loopEditFirst = stepIdx;
        displayMessage(
            `Track ${state.currentTrack + 1} Loop`,
            `Start: ${stepIdx + 1}`,
            "Tap end step...",
            ""
        );
    } else {
        /* Second tap - set end point */
        const startStep = Math.min(state.loopEditFirst, stepIdx);
        const endStep = Math.max(state.loopEditFirst, stepIdx);
        const pattern = getCurrentPattern(state.currentTrack);
        pattern.loopStart = startStep;
        pattern.loopEnd = endStep;
        setParam(`track_${state.currentTrack}_loop_start`, String(startStep));
        setParam(`track_${state.currentTrack}_loop_end`, String(endStep));
        displayMessage(
            `Track ${state.currentTrack + 1} Loop`,
            `Set: ${startStep + 1}-${endStep + 1}`,
            `${endStep - startStep + 1} steps`,
            ""
        );
        state.loopEditFirst = -1;
        markDirty();
    }
    updateLEDs();
}

/* ============ LED Updates ============ */

/**
 * Update ALL LEDs for loop mode
 * This mode owns all LEDs - nothing shared
 */
export function updateLEDs() {
    const pattern = getCurrentPattern(state.currentTrack);
    const trackColor = TRACK_COLORS[state.currentTrack];

    /* Steps - show loop range */
    for (let i = 0; i < NUM_STEPS; i++) {
        let color = Black;

        /* Steps within loop range */
        if (i >= pattern.loopStart && i <= pattern.loopEnd) {
            color = LightGrey;
        }

        /* Loop start/end points */
        if (i === pattern.loopStart || i === pattern.loopEnd) {
            color = trackColor;
        }

        /* Currently selected first point */
        if (state.loopEditFirst >= 0 && i === state.loopEditFirst) {
            color = Cyan;
        }

        setLED(MoveSteps[i], color);
    }

    /* Pads owned by track.js coordinator */

    /* Knobs - all off */
    for (let i = 0; i < 8; i++) {
        setButtonLED(MoveKnobLEDs[i], Black);
    }

    /* Track buttons - all off (no track selection in this mode) */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }

    /* Transport - play/rec reflect global state, loop is highlighted */
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);
    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
    setButtonLED(MoveLoop, Cyan);

    /* Capture and Back - off */
    setButtonLED(MoveCapture, Black);
    setButtonLED(MoveBack, Black);
}

/* ============ Display ============ */

export function updateDisplayContent() {
    const pattern = getCurrentPattern(state.currentTrack);
    displayMessage(
        "LOOP EDIT",
        `Track ${state.currentTrack + 1}`,
        `Loop: ${pattern.loopStart + 1}-${pattern.loopEnd + 1}`,
        "Tap start & end steps"
    );
}

/* ============ Lifecycle ============ */

export function onEnter() {
    state.loopEditFirst = -1;
    updateDisplayContent();
}

export function onExit() {
    state.loopEditFirst = -1;
}
