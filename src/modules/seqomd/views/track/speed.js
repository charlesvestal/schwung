/*
 * Track View - Speed Mode
 * Adjust track speed via jog wheel
 *
 * This mode is fully self-contained:
 * - Owns ALL LEDs
 * - Handles jog wheel input
 * - No track selection, no other controls
 */

import {
    Cyan, BrightGreen, BrightRed, Black, White, MoveMainKnob,
    MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack,
    MoveStep5UI
} from "../../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../../shared/input_filter.mjs";
import { NUM_STEPS, MoveKnobLEDs, SPEED_OPTIONS } from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import { setParam } from '../../lib/helpers.js';
import { markDirty } from '../../lib/persistence.js';

/* ============ Input Handling ============ */

/**
 * Handle input in speed mode
 * Only responds to jog wheel - all other input ignored
 */
export function onInput(data) {
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Jog wheel turn - adjust speed */
    if (isCC && note === MoveMainKnob) {
        let speedIdx = state.tracks[state.currentTrack].speedIndex;
        if (velocity >= 1 && velocity <= 63) {
            speedIdx = Math.min(speedIdx + 1, SPEED_OPTIONS.length - 1);
        } else if (velocity >= 65 && velocity <= 127) {
            speedIdx = Math.max(speedIdx - 1, 0);
        }
        state.tracks[state.currentTrack].speedIndex = speedIdx;
        setParam(`track_${state.currentTrack}_speed`, String(SPEED_OPTIONS[speedIdx].mult));
        markDirty();
        updateDisplayContent();
        return true;
    }

    /* Ignore all other input - mode is focused */
    return true;
}

/* ============ LED Updates ============ */

/**
 * Update ALL LEDs for speed mode
 * This mode owns all LEDs - nothing shared
 */
export function updateLEDs() {
    /* Steps - all black, playhead handled separately */
    for (let i = 0; i < NUM_STEPS; i++) {
        const isPlayhead = state.playing && i === state.currentPlayStep;
        setLED(MoveSteps[i], isPlayhead ? White : Black);
    }

    /* Step 5 UI lit to show we're in speed mode */
    setButtonLED(MoveStep5UI, White);

    /* Pads owned by track.js coordinator */

    /* Knobs - all off */
    for (let i = 0; i < 8; i++) {
        setButtonLED(MoveKnobLEDs[i], Black);
    }

    /* Track buttons - all off (no track selection in this mode) */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }

    /* Transport - only play/rec reflect global state */
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);
    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
    setButtonLED(MoveLoop, Black);

    /* Capture off, Back lit to show exit option */
    setButtonLED(MoveCapture, Black);
    setButtonLED(MoveBack, White);
}

/* ============ Playhead ============ */

/**
 * Lightweight playhead update - only updates the two step LEDs that changed
 */
export function updatePlayhead(oldStep, newStep) {
    /* Restore old step to black */
    if (oldStep >= 0 && oldStep < NUM_STEPS) {
        setLED(MoveSteps[oldStep], Black);
    }
    /* Set new step to playhead */
    if (newStep >= 0 && newStep < NUM_STEPS) {
        setLED(MoveSteps[newStep], White);
    }
}

/* ============ Display ============ */

export function updateDisplayContent() {
    const speedName = SPEED_OPTIONS[state.tracks[state.currentTrack].speedIndex].name;
    displayMessage(
        "SPEED MODE",
        `Track ${state.currentTrack + 1}`,
        `Speed: ${speedName}`,
        "Jog wheel to adjust"
    );
}

/* ============ Lifecycle ============ */

export function onEnter() {
    updateDisplayContent();
}

export function onExit() {
    /* Changes saved on each input, nothing to do here */
}
