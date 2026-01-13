/*
 * Track View - Swing Mode
 * Adjust track swing via jog wheel
 *
 * This mode is fully self-contained:
 * - Owns ALL LEDs
 * - Handles jog wheel input
 * - No track selection, no other controls
 */

import {
    VividYellow, BrightGreen, BrightRed, Black, White, MoveMainKnob,
    MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack,
    MoveStep7UI
} from "../../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../../shared/input_filter.mjs";
import { NUM_STEPS, MoveKnobLEDs } from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import { setParam, clearStepLEDs, clearKnobLEDs, clearTrackButtonLEDs, updateStandardTransportLEDs, updatePlayheadLED } from '../../lib/helpers.js';
import { markDirty } from '../../lib/persistence.js';

/* ============ Input Handling ============ */

/**
 * Handle input in swing mode
 * Only responds to jog wheel - all other input ignored
 */
export function onInput(data) {
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Jog wheel turn - adjust swing */
    if (isCC && note === MoveMainKnob) {
        let swing = state.tracks[state.currentTrack].swing;
        if (velocity >= 1 && velocity <= 63) {
            swing = Math.min(swing + 1, 100);
        } else if (velocity >= 65 && velocity <= 127) {
            swing = Math.max(swing - 1, 0);
        }
        state.tracks[state.currentTrack].swing = swing;
        setParam(`track_${state.currentTrack}_swing`, String(swing));
        markDirty();
        updateDisplayContent();
        return true;
    }

    /* Ignore all other input - mode is focused */
    return true;
}

/* ============ LED Updates ============ */

/**
 * Update ALL LEDs for swing mode
 * This mode owns all LEDs - nothing shared
 */
export function updateLEDs() {
    /* Steps - clear with playhead */
    clearStepLEDs(true);

    /* Step 7 UI lit to show we're in swing mode */
    setButtonLED(MoveStep7UI, White);

    /* Pads owned by track.js coordinator */

    /* Knobs - all off */
    clearKnobLEDs();

    /* Track buttons - all off (no track selection in this mode) */
    clearTrackButtonLEDs();

    /* Transport - standard pattern with Back lit */
    updateStandardTransportLEDs();
}

/* ============ Playhead ============ */

/**
 * Lightweight playhead update - only updates the two step LEDs that changed
 */
export function updatePlayhead(oldStep, newStep) {
    updatePlayheadLED(oldStep, newStep);
}

/* ============ Display ============ */

export function updateDisplayContent() {
    const swing = state.tracks[state.currentTrack].swing;
    displayMessage(
        "SWING MODE",
        `Track ${state.currentTrack + 1}`,
        `Swing: ${swing}%`,
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
