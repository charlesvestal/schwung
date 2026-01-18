/*
 * Track View - Generate Mode
 * Pattern generator with algorithmic approaches
 *
 * Entry: Shift + Step 14
 * Exit: Back button or Jog click (after generating)
 *
 * Jog wheel: Select style
 * Knobs 1-8: Adjust parameters
 * Jog press: Generate pattern
 */

import {
    Black, White, Cyan, HotMagenta, Purple, VividYellow, BrightGreen, LightGrey,
    MoveMainKnob, MoveMainButton, MoveSteps,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveStep14UI
} from "../../lib/shared-constants.js";

import { setLED, setButtonLED } from "../../lib/shared-input.js";

import {
    GENERATE_STYLES, GENERATOR_SCALES, GENERATOR_ROOTS, NUM_PATTERNS,
    MoveKnobLEDs, TRACK_COLORS
} from '../../lib/constants.js';

import { state, displayMessage } from '../../lib/state.js';
import { setParam, clearStepLEDs, clearTrackButtonLEDs, updateStandardTransportLEDs, syncAllTracksToDSP } from '../../lib/helpers.js';
import { generatePattern, patternHasContent } from '../../lib/generator.js';
import { detectScale } from '../../lib/scale_detection.js';
import { markDirty } from '../../lib/persistence.js';

/* ============ Local State ============ */

let styleDisplayTimeout = null;
let hasGeneratedThisSession = false;  // Track if we've already generated once
let generatedPatternIdx = -1;         // Which pattern we generated to

/* ============ Display Helpers ============ */

function formatParamValue(paramName, value) {
    switch (paramName) {
        case 'density':
            return `${value}%`;
        case 'scale':
            return GENERATOR_SCALES[value].short;
        case 'root':
            return GENERATOR_ROOTS[value].short;
        case 'octave':
            return `C${value}`;
        case 'range':
            return `${value}oct`;
        default:
            return String(value);
    }
}

function updateParamsDisplay() {
    const p = state.generateParams;

    // 4 lines, 2 params each
    displayMessage(
        `Len:${p.length}  Dns:${p.density}%`,
        `Voc:${p.voices}  Scl:${GENERATOR_SCALES[p.scale].short}`,
        `Oct:${p.octave}  Rng:${p.range}`,
        `Roo:${GENERATOR_ROOTS[p.root].short}  Var:${p.variation}`
    );
}

function showStyleBriefly() {
    const style = GENERATE_STYLES[state.generateStyle];
    displayMessage(
        `>>> ${style.name.toUpperCase()} <<<`,
        "",
        "",
        ""
    );

    // Clear any existing timeout
    if (styleDisplayTimeout) {
        clearTimeout(styleDisplayTimeout);
    }

    // Return to params display after 800ms
    styleDisplayTimeout = setTimeout(() => {
        updateParamsDisplay();
        styleDisplayTimeout = null;
    }, 800);
}

/* ============ Input Handling ============ */

export function onInput(data) {
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Jog wheel turn - change style */
    if (isCC && note === MoveMainKnob) {
        let styleIdx = state.generateStyle;

        if (velocity >= 1 && velocity <= 63) {
            styleIdx = (styleIdx + 1) % GENERATE_STYLES.length;
        } else if (velocity >= 65 && velocity <= 127) {
            styleIdx = (styleIdx - 1 + GENERATE_STYLES.length) % GENERATE_STYLES.length;
        }

        state.generateStyle = styleIdx;
        showStyleBriefly();
        return true;
    }

    /* Jog wheel press - generate! */
    if (isCC && note === MoveMainButton && velocity > 0) {
        doGenerate();
        return true;
    }

    /* Knobs 1-8 - adjust parameters */
    const knobs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];
    if (isCC && knobs.includes(note)) {
        const knobIdx = knobs.indexOf(note);
        handleKnob(knobIdx, velocity);
        return true;
    }

    /* Ignore all other input - mode is focused */
    return true;
}

function handleKnob(knobIdx, velocity) {
    const p = state.generateParams;
    const delta = velocity >= 1 && velocity <= 63 ? 1 : -1;

    switch (knobIdx) {
        case 0: // Length
            p.length = Math.max(1, Math.min(64, p.length + delta));
            break;
        case 1: // Density
            p.density = Math.max(5, Math.min(100, p.density + delta * 5));
            break;
        case 2: // Voices
            p.voices = Math.max(1, Math.min(7, p.voices + delta));
            break;
        case 3: // Scale
            p.scale = (p.scale + delta + GENERATOR_SCALES.length) % GENERATOR_SCALES.length;
            break;
        case 4: // Root
            p.root = (p.root + delta + GENERATOR_ROOTS.length) % GENERATOR_ROOTS.length;
            break;
        case 5: // Octave
            p.octave = Math.max(1, Math.min(6, p.octave + delta));
            break;
        case 6: // Range
            p.range = Math.max(1, Math.min(4, p.range + delta));
            break;
        case 7: // Variation
            p.variation = Math.max(0, Math.min(127, p.variation + delta * 4));
            break;
    }

    updateParamsDisplay();
    updateKnobLEDs();
}

/* ============ Pattern Generation ============ */

function doGenerate() {
    const track = state.tracks[state.currentTrack];
    const currentPatternIdx = track.currentPattern < 0 ? 0 : track.currentPattern;
    const currentPattern = track.patterns[currentPatternIdx];

    // Determine target pattern
    let targetPatternIdx;

    if (hasGeneratedThisSession && generatedPatternIdx >= 0) {
        // Re-generate on the same pattern we used before (audition mode)
        targetPatternIdx = generatedPatternIdx;
    } else if (patternHasContent(currentPattern)) {
        // First generate and current pattern has content - find empty slot
        targetPatternIdx = currentPatternIdx;
        let foundEmpty = false;
        for (let i = currentPatternIdx + 1; i < NUM_PATTERNS; i++) {
            if (!patternHasContent(track.patterns[i])) {
                targetPatternIdx = i;
                foundEmpty = true;
                break;
            }
        }
        if (!foundEmpty) {
            // Wrap around and search from beginning
            for (let i = 0; i < currentPatternIdx; i++) {
                if (!patternHasContent(track.patterns[i])) {
                    targetPatternIdx = i;
                    foundEmpty = true;
                    break;
                }
            }
        }
        if (!foundEmpty) {
            // All patterns full, use next pattern (will overwrite)
            targetPatternIdx = (currentPatternIdx + 1) % NUM_PATTERNS;
        }
    } else {
        // Current pattern is empty, generate here
        targetPatternIdx = currentPatternIdx;
    }

    // Remember this pattern for re-generation
    hasGeneratedThisSession = true;
    generatedPatternIdx = targetPatternIdx;

    // Get detected scale for "Detected" option
    const detectedScale = detectScale(state.tracks, state.chordFollow);

    // Generate the pattern
    const generatedSteps = generatePattern(
        state.generateParams,
        state.generateStyle,
        detectedScale
    );

    // Apply to target pattern
    track.patterns[targetPatternIdx].steps = generatedSteps;

    // Always set track length to match generated pattern
    track.trackLength = state.generateParams.length;
    setParam(`track_${state.currentTrack}_length`, String(track.trackLength));

    // Switch to target pattern if different
    if (targetPatternIdx !== currentPatternIdx) {
        track.currentPattern = targetPatternIdx;
        setParam(`track_${state.currentTrack}_pattern`, String(targetPatternIdx));
    }

    // Reset to page 0 so user sees start of generated pattern
    state.currentPage = 0;

    // Sync to DSP
    syncAllTracksToDSP();
    markDirty();

    // Show confirmation
    const style = GENERATE_STYLES[state.generateStyle];
    displayMessage(
        "GENERATED!",
        `${style.name} -> Pattern ${targetPatternIdx + 1}`,
        `${state.generateParams.length} steps`,
        ""
    );

    // Flash step LEDs briefly
    flashGeneratedSteps(generatedSteps);
}

function flashGeneratedSteps(steps) {
    const trackColor = TRACK_COLORS[state.currentTrack];

    // Show generated steps
    for (let i = 0; i < 16; i++) {
        if (steps[i] && steps[i].notes && steps[i].notes.length > 0) {
            setLED(MoveSteps[i], trackColor);
        } else {
            setLED(MoveSteps[i], Black);
        }
    }

    // Return to params display after a moment
    setTimeout(() => {
        updateParamsDisplay();
        updateLEDs();
    }, 1500);
}

/* ============ LED Updates ============ */

export function updateLEDs() {
    updateStepLEDs();
    updateKnobLEDs();
    clearTrackButtonLEDs();
    updateStandardTransportLEDs();
}

function updateStepLEDs() {
    // Clear all step LEDs
    clearStepLEDs(false);

    // Light up Step 14 UI to show we're in generate mode
    setButtonLED(MoveStep14UI, HotMagenta);
}

function updateKnobLEDs() {
    // Color scheme for parameters:
    // 1-2: Cyan (rhythm: length, density)
    // 3: Green (voices)
    // 4-5: Purple (scale, root)
    // 6-7: Yellow (octave, range)
    // 8: HotMagenta (variation)

    setButtonLED(MoveKnobLEDs[0], Cyan);
    setButtonLED(MoveKnobLEDs[1], Cyan);
    setButtonLED(MoveKnobLEDs[2], BrightGreen);
    setButtonLED(MoveKnobLEDs[3], Purple);
    setButtonLED(MoveKnobLEDs[4], Purple);
    setButtonLED(MoveKnobLEDs[5], VividYellow);
    setButtonLED(MoveKnobLEDs[6], VividYellow);
    setButtonLED(MoveKnobLEDs[7], HotMagenta);
}

/* ============ Display ============ */

export function updateDisplayContent() {
    updateParamsDisplay();
}

/* ============ Lifecycle ============ */

export function onEnter() {
    updateParamsDisplay();
}

export function onExit() {
    // Clear any pending timeout
    if (styleDisplayTimeout) {
        clearTimeout(styleDisplayTimeout);
        styleDisplayTimeout = null;
    }
    // Reset session state for next time
    hasGeneratedThisSession = false;
    generatedPatternIdx = -1;
}
