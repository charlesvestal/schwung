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
} from "../../lib/shared-constants.js";

import { setLED, setButtonLED } from "../../lib/shared-input.js";
import { NUM_STEPS, MoveKnobLEDs, SPEED_OPTIONS } from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import { setParam, clearStepLEDs, clearKnobLEDs, clearTrackButtonLEDs, updateStandardTransportLEDs, handleJogWheelTrackParam } from '../../lib/helpers.js';

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
        handleJogWheelTrackParam(
            velocity,
            'speedIndex',
            'speed',
            { max: SPEED_OPTIONS.length - 1 },
            (idx) => SPEED_OPTIONS[idx].mult,
            updateDisplayContent
        );
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
    /* Steps - clear with playhead */
    clearStepLEDs(true);

    /* Step 5 UI lit to show we're in speed mode */
    setButtonLED(MoveStep5UI, White);

    /* Pads owned by track.js coordinator */

    /* Knobs - all off */
    clearKnobLEDs();

    /* Track buttons - all off (no track selection in this mode) */
    clearTrackButtonLEDs();

    /* Transport - standard pattern with Back lit */
    updateStandardTransportLEDs();
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
