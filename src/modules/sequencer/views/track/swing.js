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
    VividYellow, Black, MoveMainKnob,
    MovePads, MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack
} from "../../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../../shared/input_filter.mjs";
import { NUM_STEPS, MoveKnobLEDs } from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import { setParam } from '../../lib/helpers.js';

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
    /* Steps - only step 7 lit */
    for (let i = 0; i < NUM_STEPS; i++) {
        setLED(MoveSteps[i], i === 6 ? VividYellow : Black);
    }

    /* Pads - all off */
    for (let i = 0; i < 32; i++) {
        setLED(MovePads[i], Black);
    }

    /* Knobs - all off */
    for (let i = 0; i < 8; i++) {
        setButtonLED(MoveKnobLEDs[i], Black);
    }

    /* Track buttons - all off (no track selection in this mode) */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }

    /* Transport - only play/rec reflect global state */
    setButtonLED(MovePlay, state.playing ? 127 : Black);
    setButtonLED(MoveRec, state.recording ? 127 : Black);
    setButtonLED(MoveLoop, Black);

    /* Capture and Back - off */
    setButtonLED(MoveCapture, Black);
    setButtonLED(MoveBack, Black);
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
    /* No cleanup needed */
}
