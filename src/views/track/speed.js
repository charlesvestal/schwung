/*
 * Track View - Scale Mode (formerly Speed Mode)
 * Adjust track speed, length, reset, and gate via knobs
 *
 * Knob 1: Speed (1/4x - 4x)
 * Knob 2: Track Length (1-64 steps)
 * Knob 3: Track Reset (INF, 1-256 steps)
 * Knob 4: Gate (1-100%)
 *
 * This mode is fully self-contained:
 * - Owns ALL LEDs
 * - Handles knob input
 * - No track selection, no other controls
 */

import {
    Cyan, BrightGreen, Purple, BrightRed, Black, White, VividYellow,
    MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack,
    MoveStep5UI,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4
} from "../../lib/shared-constants.js";

import { setLED, setButtonLED } from "../../lib/shared-input.js";
import {
    STEPS_PER_PAGE, NUM_STEPS, MoveKnobLEDs, SPEED_OPTIONS,
    DEFAULT_TRACK_LENGTH, RESET_INF, MAX_RESET,
    DEFAULT_GATE, MIN_GATE, MAX_GATE
} from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import {
    setParam, clearStepLEDs, clearKnobLEDs, clearTrackButtonLEDs,
    updateStandardTransportLEDs
} from '../../lib/helpers.js';
import { markDirty } from '../../lib/persistence.js';

/* ============ Input Handling ============ */

/**
 * Handle input in scale mode
 * Responds to knobs 1-3
 */
export function onInput(data) {
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Knob 1 - Speed */
    if (isCC && note === MoveKnob1) {
        handleSpeedKnob(velocity);
        return true;
    }

    /* Knob 2 - Track Length */
    if (isCC && note === MoveKnob2) {
        handleLengthKnob(velocity);
        return true;
    }

    /* Knob 3 - Track Reset */
    if (isCC && note === MoveKnob3) {
        handleResetKnob(velocity);
        return true;
    }

    /* Knob 4 - Gate */
    if (isCC && note === MoveKnob4) {
        handleGateKnob(velocity);
        return true;
    }

    /* Ignore all other input - mode is focused */
    return true;
}

/**
 * Handle speed knob (Knob 1)
 */
function handleSpeedKnob(velocity) {
    const track = state.tracks[state.currentTrack];
    let speedIdx = track.speedIndex;

    if (velocity >= 1 && velocity <= 63) {
        speedIdx = Math.min(speedIdx + 1, SPEED_OPTIONS.length - 1);
    } else if (velocity >= 65 && velocity <= 127) {
        speedIdx = Math.max(speedIdx - 1, 0);
    }

    track.speedIndex = speedIdx;
    setParam(`track_${state.currentTrack}_speed`, String(SPEED_OPTIONS[speedIdx].mult));
    markDirty();
    updateDisplayContent();
}

/**
 * Handle track length knob (Knob 2)
 */
function handleLengthKnob(velocity) {
    const track = state.tracks[state.currentTrack];
    let length = track.trackLength;

    if (velocity >= 1 && velocity <= 63) {
        length = Math.min(length + 1, NUM_STEPS);
    } else if (velocity >= 65 && velocity <= 127) {
        length = Math.max(length - 1, 1);
    }

    track.trackLength = length;
    setParam(`track_${state.currentTrack}_length`, String(length));
    markDirty();
    updateDisplayContent();
}

/**
 * Handle track reset knob (Knob 3)
 */
function handleResetKnob(velocity) {
    const track = state.tracks[state.currentTrack];
    let reset = track.resetLength;

    if (velocity >= 1 && velocity <= 63) {
        /* Clockwise: INF -> 1 -> 2 -> ... -> 256 */
        if (reset === RESET_INF) {
            reset = 1;
        } else {
            reset = Math.min(reset + 1, MAX_RESET);
        }
    } else if (velocity >= 65 && velocity <= 127) {
        /* Counter-clockwise: 256 -> ... -> 1 -> INF */
        if (reset === 1) {
            reset = RESET_INF;
        } else if (reset > 1) {
            reset = reset - 1;
        }
        /* If already INF, stay at INF */
    }

    track.resetLength = reset;
    setParam(`track_${state.currentTrack}_reset`, String(reset));
    markDirty();
    updateDisplayContent();
}

/**
 * Handle gate knob (Knob 4)
 */
function handleGateKnob(velocity) {
    const track = state.tracks[state.currentTrack];
    let gate = track.gate;

    if (velocity >= 1 && velocity <= 63) {
        gate = Math.min(gate + 1, MAX_GATE);
    } else if (velocity >= 65 && velocity <= 127) {
        gate = Math.max(gate - 1, MIN_GATE);
    }

    track.gate = gate;
    setParam(`track_${state.currentTrack}_gate`, String(gate));
    markDirty();
    updateDisplayContent();
}

/* ============ LED Updates ============ */

/**
 * Update ALL LEDs for scale mode
 * This mode owns all LEDs - nothing shared
 */
export function updateLEDs() {
    /* Steps - clear with playhead */
    clearStepLEDs(true);

    /* Step 5 UI lit to show we're in scale mode */
    setButtonLED(MoveStep5UI, White);

    /* Pads owned by track.js coordinator */

    /* Knobs - show which are active */
    clearKnobLEDs();
    setButtonLED(MoveKnobLEDs[0], Cyan);        /* Speed - Cyan */
    setButtonLED(MoveKnobLEDs[1], BrightGreen); /* Length - Green */
    setButtonLED(MoveKnobLEDs[2], Purple);      /* Reset - Purple */
    setButtonLED(MoveKnobLEDs[3], VividYellow); /* Gate - Yellow */

    /* Track buttons - all off (no track selection in this mode) */
    clearTrackButtonLEDs();

    /* Transport - standard pattern with Back lit */
    updateStandardTransportLEDs();
}

/* ============ Display ============ */

export function updateDisplayContent() {
    const track = state.tracks[state.currentTrack];
    const speedName = SPEED_OPTIONS[track.speedIndex].name;
    const lengthStr = String(track.trackLength);
    const resetStr = track.resetLength === RESET_INF ? "INF" : String(track.resetLength);
    const gateStr = String(track.gate) + "%";

    displayMessage(
        "SCALE MODE",
        `Spd: ${speedName}   Len: ${lengthStr}`,
        `Rst: ${resetStr}  Gate: ${gateStr}`,
        ""
    );
}

/* ============ Lifecycle ============ */

export function onEnter() {
    updateDisplayContent();
}

export function onExit() {
    /* Changes saved on each input, nothing to do here */
}
