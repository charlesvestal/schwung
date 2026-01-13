/*
 * Track View - Channel Mode
 * Adjust track MIDI channel via jog wheel
 *
 * This mode is fully self-contained:
 * - Owns ALL LEDs
 * - Handles jog wheel input
 * - No track selection, no other controls
 */

import {
    BrightGreen, BrightRed, Black, White, MoveMainKnob,
    MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack,
    MoveStep2UI
} from "../../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../../shared/input_filter.mjs";
import { NUM_STEPS, MoveKnobLEDs } from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import { setParam, clearStepLEDs, clearKnobLEDs, clearTrackButtonLEDs, updateStandardTransportLEDs } from '../../lib/helpers.js';
import { markDirty } from '../../lib/persistence.js';

/* ============ Input Handling ============ */

/**
 * Handle input in channel mode
 * Only responds to jog wheel - all other input ignored
 */
export function onInput(data) {
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Jog wheel turn - adjust channel */
    if (isCC && note === MoveMainKnob) {
        let ch = state.tracks[state.currentTrack].channel;
        if (velocity >= 1 && velocity <= 63) {
            ch = (ch + 1) % 16;
        } else if (velocity >= 65 && velocity <= 127) {
            ch = (ch - 1 + 16) % 16;
        }
        state.tracks[state.currentTrack].channel = ch;
        setParam(`track_${state.currentTrack}_channel`, String(ch));
        markDirty();
        updateDisplayContent();
        return true;
    }

    /* Ignore all other input - mode is focused */
    return true;
}

/* ============ LED Updates ============ */

/**
 * Update ALL LEDs for channel mode
 * This mode owns all LEDs - nothing shared
 */
export function updateLEDs() {
    /* Steps - clear with playhead */
    clearStepLEDs(true);

    /* Step 2 UI lit to show we're in channel mode */
    setButtonLED(MoveStep2UI, White);

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
    const ch = state.tracks[state.currentTrack].channel + 1;
    displayMessage(
        "CHANNEL MODE",
        `Track ${state.currentTrack + 1}`,
        `Channel: ${ch}`,
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
