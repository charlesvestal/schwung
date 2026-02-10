/*
 * Track View - Shared Utilities
 * Re-exports commonly used items for mode files
 *
 * Note: Each mode is fully self-contained and handles its own LEDs/input.
 * This file only provides convenient re-exports to reduce import boilerplate.
 */

import {
    Black, White, LightGrey, Navy, Cyan, Purple, BrightGreen, BrightRed,
    MoveSteps, MoveTracks, MoveLoop, MoveCapture, MovePlay, MoveRec, MoveBack
} from "../../lib/shared-constants.js";

import { setLED, setButtonLED } from "../../lib/shared-input.js";

import {
    NUM_TRACKS, NUM_STEPS,
    MoveKnobLEDs, TRACK_COLORS, TRACK_COLORS_DIM
} from '../../lib/constants.js';

import { state, displayMessage } from '../../lib/state.js';
import { getCurrentPattern } from '../../lib/helpers.js';

/* Re-export commonly used items for mode files */
export {
    Black, White, LightGrey, Navy, Cyan, Purple, BrightGreen, BrightRed,
    MoveSteps, MoveTracks, MoveLoop, MoveCapture, MovePlay, MoveRec, MoveBack,
    setLED, setButtonLED,
    NUM_TRACKS, NUM_STEPS, MoveKnobLEDs, TRACK_COLORS, TRACK_COLORS_DIM,
    state, displayMessage, getCurrentPattern
};
