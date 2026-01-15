/*
 * Master View
 * Transpose control with piano display and scale detection
 */

import {
    Black, White, LightGrey, DarkGrey, BrightGreen, Cyan, BrightRed, VividYellow, Purple, DarkPurple,
    MoveSteps, MovePads, MoveTracks, MoveLoop, MovePlay, MoveRec, MoveCapture, MoveBack,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveUp, MoveDown, MoveDelete, MoveMainKnob,
    MoveStep1UI, MoveStep2UI, MoveStep3UI, MoveStep4UI, MoveStep5UI, MoveStep6UI, MoveStep7UI, MoveStep8UI,
    MoveStep9UI, MoveStep10UI, MoveStep11UI, MoveStep12UI, MoveStep13UI, MoveStep14UI, MoveStep15UI, MoveStep16UI
} from "../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../shared/input_filter.mjs";

import { NUM_TRACKS, NUM_STEPS, MoveKnobLEDs, TRACK_COLORS, TRACK_COLORS_DIM } from '../lib/constants.js';
import * as spark from './master/spark.js';
import { state, displayMessage } from '../lib/state.js';
import { setParam, getParam, syncTransposeSequenceToDSP, updateBpm } from '../lib/helpers.js';
import { isRoot, isInScale, NOTE_NAMES, SCALES } from '../lib/scale_detection.js';
import {
    setTransposeStep, removeTransposeStep, getTransposeStep, adjustTransposeDuration,
    getCurrentStepIndex, getStepCount, formatDuration, MAX_TRANSPOSE_STEPS
} from '../lib/transpose_sequence.js';
import { markDirty } from '../lib/persistence.js';

/* ============ Piano Mapping ============ */

/* Pad index to semitone (within octave) - gaps are null */
const PAD_TO_SEMITONE = [
    0,    /* Pad 0: C */
    2,    /* Pad 1: D */
    4,    /* Pad 2: E */
    5,    /* Pad 3: F */
    7,    /* Pad 4: G */
    9,    /* Pad 5: A */
    11,   /* Pad 6: B */
    12,   /* Pad 7: C+1 (octave) */
    null, /* Pad 8: gap */
    1,    /* Pad 9: C# */
    3,    /* Pad 10: D# */
    null, /* Pad 11: gap */
    6,    /* Pad 12: F# */
    8,    /* Pad 13: G# */
    10,   /* Pad 14: A# */
    null  /* Pad 15: gap */
];

/* Which pads are white keys vs black keys */
const WHITE_KEY_PADS = [0, 1, 2, 3, 4, 5, 6, 7];
const BLACK_KEY_PADS = [9, 10, 12, 13, 14];
const GAP_PADS = [8, 11, 15];

/* ============ Scale Detection from DSP ============ */

/**
 * Read detected scale from DSP and update state.detectedScale
 * DSP provides root (0-11 or -1) and scale name string
 */
function updateDetectedScaleFromDSP() {
    const rootStr = getParam('detected_scale_root');
    const nameStr = getParam('detected_scale_name');

    const root = rootStr ? parseInt(rootStr, 10) : -1;

    if (root < 0 || !nameStr) {
        state.detectedScale = null;
        return;
    }

    /* Find scale template by name to get the note intervals */
    const scaleTemplate = SCALES.find(s => s.name === nameStr);
    if (!scaleTemplate) {
        state.detectedScale = null;
        return;
    }

    /* Build scale notes set (transposed to root) */
    const scaleNotes = new Set(scaleTemplate.notes.map(n => (n + root) % 12));

    state.detectedScale = {
        root: root,
        scaleName: nameStr,
        scaleNotes: scaleNotes
    };
}

/**
 * Get display name for detected scale
 */
function getScaleDisplayName() {
    if (!state.detectedScale) return 'No scale';
    return `${NOTE_NAMES[state.detectedScale.root]} ${state.detectedScale.scaleName}`;
}

/* ============ View Interface ============ */

/* BPM edit mode flag (local to master view) */
let bpmEditMode = false;

/* Transpose spark mode flag (local to master view) */
let transposeSparkMode = false;

/**
 * Called when entering master view
 */
export function onEnter() {
    /* Read scale detection from DSP */
    updateDetectedScaleFromDSP();
    state.heldTransposeStep = -1;
    state.heldLiveTransposePad = -1;
    bpmEditMode = false;
    transposeSparkMode = false;
    updateDisplayContent();
}

/**
 * Called when exiting master view
 */
export function onExit() {
    state.heldTransposeStep = -1;
    state.heldLiveTransposePad = -1;
    /* Clear live transpose when exiting master view */
    setParam("live_transpose", "0");
    bpmEditMode = false;
    transposeSparkMode = false;
    state.transposeSparkSelectedSteps.clear();
}

/**
 * Handle MIDI input for master view
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Capture button handling */
    if (isCC && note === MoveCapture) {
        if (velocity > 0) {
            /* Capture PRESS */
            if (transposeSparkMode) {
                /* Already in spark mode - exit it */
                transposeSparkMode = false;
                state.transposeSparkSelectedSteps.clear();
                spark.onExit();
                updateLEDs();
                updateDisplayContent();
                return true;
            } else {
                /* Normal mode - set held flag for preview */
                state.captureHeld = true;
                updateLEDs();  /* Show spark preview */
            }
        } else {
            /* Capture RELEASE - just clear held flag (don't exit spark mode) */
            state.captureHeld = false;
            if (!transposeSparkMode) {
                updateLEDs();  /* Restore normal view if not in spark mode */
            }
        }
        return true;
    }

    /* Step 5 (note 20) with shift - enter BPM mode */
    if (isNote && note === 20 && state.shiftHeld) {
        if (isNoteOn && velocity > 0) {
            bpmEditMode = true;
            updateDisplayContent();
            updateLEDs();
        }
        return true;
    }

    /* Capture + Step enters transpose spark mode */
    if (!transposeSparkMode && state.captureHeld && isNote &&
        note >= 16 && note <= 31 && isNoteOn && velocity > 0 && !bpmEditMode) {
        const stepIdx = note - 16;
        transposeSparkMode = true;
        state.transposeSparkSelectedSteps.add(stepIdx);
        spark.onEnter();
        spark.updateLEDs();
        spark.updateDisplayContent();
        return true;
    }

    /* Back button exits spark mode */
    if (transposeSparkMode && isCC && note === MoveBack && velocity > 0) {
        transposeSparkMode = false;
        state.transposeSparkSelectedSteps.clear();
        spark.onExit();
        updateLEDs();
        updateDisplayContent();
        return true;
    }

    /* Route all input to spark module when in spark mode */
    if (transposeSparkMode) {
        return spark.onInput(data);
    }

    /* Step buttons - transpose sequence (only when not in BPM mode and Capture not held) */
    if (isNote && note >= 16 && note <= 31 && !bpmEditMode && !state.captureHeld) {
        return handleStepButton(note - 16, isNoteOn, velocity);
    }

    /* Capture held without pressing step - just ignore other step button presses */
    if (isNote && note >= 16 && note <= 31 && state.captureHeld && !transposeSparkMode) {
        return true;  /* Consume the event */
    }

    /* Pads - piano (rows 3-4) and chord follow (row 1) */
    if (isNote && note >= 68 && note <= 99) {
        const padIdx = note - 68;
        if (isNoteOn && velocity > 0) {
            return handlePadPress(padIdx);
        } else {
            return handlePadRelease(padIdx);
        }
    }

    /* MoveUp - octave up */
    if (isCC && note === MoveUp && velocity > 0) {
        if (state.transposeOctaveOffset < 2) {
            state.transposeOctaveOffset++;
            updateDisplayContent();
            updatePadLEDs();
        }
        return true;
    }

    /* MoveDown - octave down */
    if (isCC && note === MoveDown && velocity > 0) {
        if (state.transposeOctaveOffset > -2) {
            state.transposeOctaveOffset--;
            updateDisplayContent();
            updatePadLEDs();
        }
        return true;
    }

    /* MoveDelete + step - handled in step button */
    if (isCC && note === MoveDelete) {
        state.deleteHeld = velocity > 0;
        return true;
    }

    /* Knob 1 - adjust duration when holding step */
    if (isCC && note === MoveKnob1 && state.heldTransposeStep >= 0) {
        const step = getTransposeStep(state.heldTransposeStep);
        if (step) {
            let delta = 0;
            if (velocity >= 1 && velocity <= 63) {
                delta = 1;
            } else if (velocity >= 65 && velocity <= 127) {
                delta = -1;
            }
            if (delta !== 0) {
                const newDur = adjustTransposeDuration(state.heldTransposeStep, delta);
                if (newDur !== null) {
                    /* Sync transpose sequence to DSP */
                    syncTransposeSequenceToDSP();
                    markDirty();
                    updateDisplayContent();
                    updateStepLEDs();
                }
            }
        }
        return true;
    }

    /* Jog wheel handling */
    if (isCC && note === MoveMainKnob) {
        /* BPM mode: adjust BPM */
        if (bpmEditMode) {
            let delta = 0;
            if (velocity >= 1 && velocity <= 63) {
                delta = 1;
            } else if (velocity >= 65 && velocity <= 127) {
                delta = -1;
            }
            if (delta !== 0) {
                updateBpm(state.bpm + delta);
                markDirty();
                updateDisplayContent();
            }
            return true;
        }

        /* Holding step: adjust duration by larger amounts */
        if (state.heldTransposeStep >= 0) {
            const step = getTransposeStep(state.heldTransposeStep);
            if (step) {
                let delta = 0;
                if (velocity >= 1 && velocity <= 63) {
                    delta = 4; /* 1 bar */
                } else if (velocity >= 65 && velocity <= 127) {
                    delta = -4;
                }
                if (delta !== 0) {
                    adjustTransposeDuration(state.heldTransposeStep, delta);
                    /* Sync transpose sequence to DSP */
                    syncTransposeSequenceToDSP();
                    markDirty();
                    updateDisplayContent();
                    updateStepLEDs();
                }
            }
            return true;
        }

        /* Not holding step: scroll tracks for chord follow (toggle between tracks 1-8 and 9-16) */
        if (velocity >= 1 && velocity <= 63) {
            /* Clockwise: jump to tracks 9-16 */
            state.trackScrollPosition = 8;
        } else if (velocity >= 65 && velocity <= 127) {
            /* Counter-clockwise: jump to tracks 1-8 */
            state.trackScrollPosition = 0;
        }
        updateDisplayContent();
        updatePadLEDs();
        return true;
    }

    /* Loop button - toggle clock output */
    if (isCC && note === MoveLoop) {
        if (velocity > 0) {
            state.sendClock = state.sendClock ? 0 : 1;
            setParam("send_clock", String(state.sendClock));
            updateDisplayContent();
            updateLEDs();
        }
        return true;
    }

    /* Track buttons - not used in master view, consume but ignore */
    if (isCC && MoveTracks.includes(note)) {
        return true;
    }

    /* Back button - exit BPM mode if active, else let router handle */
    if (isCC && note === MoveBack && velocity > 0) {
        if (bpmEditMode) {
            bpmEditMode = false;
            updateDisplayContent();
            updateLEDs();
            return true;
        }
        return false; /* Let router handle normal back */
    }

    return false;
}

/* ============ Step Button Handling ============ */

function handleStepButton(stepIdx, isNoteOn, velocity) {
    if (isNoteOn && velocity > 0) {
        /* Check for delete */
        if (state.deleteHeld) {
            removeTransposeStep(stepIdx);
            /* Sync transpose sequence to DSP */
            syncTransposeSequenceToDSP();
            markDirty();
            updateDisplayContent();
            updateStepLEDs();
            return true;
        }

        /* Hold step for editing */
        state.heldTransposeStep = stepIdx;
        updateDisplayContent();
        updatePadLEDs();
        updateStepLEDs();
    } else {
        /* Release step */
        if (state.heldTransposeStep === stepIdx) {
            state.heldTransposeStep = -1;
            updateDisplayContent();
            updatePadLEDs();
            updateStepLEDs();
        }
    }
    return true;
}

/* ============ Pad Handling ============ */

function handlePadPress(padIdx) {
    /* Row 1 (indices 24-31): Chord follow toggle with scroll offset */
    if (padIdx >= 24) {
        const trackIdx = (padIdx - 24) + state.trackScrollPosition;
        if (trackIdx >= NUM_TRACKS) return true;  /* Out of bounds */
        state.chordFollow[trackIdx] = !state.chordFollow[trackIdx];
        /* Sync to DSP - DSP will recalculate scale detection */
        setParam(`track_${trackIdx}_chord_follow`, state.chordFollow[trackIdx] ? "1" : "0");
        markDirty();
        /* Read updated scale from DSP */
        updateDetectedScaleFromDSP();
        updateDisplayContent();
        updatePadLEDs();
        return true;
    }

    /* Row 2 (indices 16-23): Reserved - ignore */
    if (padIdx >= 16) {
        return true;
    }

    /* Rows 3-4 (indices 0-15): Piano keys */
    const semitone = PAD_TO_SEMITONE[padIdx];
    if (semitone === null) {
        return true; /* Gap pad */
    }

    /* Calculate actual transpose value relative to detected scale root (or C if no scale) */
    const rootSemitone = state.detectedScale ? state.detectedScale.root : 0;
    const transpose = (semitone - rootSemitone) + (state.transposeOctaveOffset * 12);

    /* If holding a step, set its transpose */
    if (state.heldTransposeStep >= 0) {
        const existingStep = getTransposeStep(state.heldTransposeStep);
        const duration = existingStep ? existingStep.duration : 4;
        setTransposeStep(state.heldTransposeStep, transpose, duration);
        /* Sync transpose sequence to DSP */
        syncTransposeSequenceToDSP();
        markDirty();
        updateDisplayContent();
        updateStepLEDs();
        updatePadLEDs();
    } else {
        /* No step held - activate live transpose */
        state.heldLiveTransposePad = padIdx;
        setParam("live_transpose", String(transpose));
        updateDisplayContent();
        updatePadLEDs();
    }

    return true;
}

function handlePadRelease(padIdx) {
    /* If releasing the held live transpose pad, clear it */
    if (state.heldLiveTransposePad === padIdx) {
        state.heldLiveTransposePad = -1;
        setParam("live_transpose", "0");
        updateDisplayContent();
        updatePadLEDs();
    }
    return true;
}

/* ============ LED Updates ============ */

export function updateLEDs() {
    /* Delegate to spark module if in spark mode */
    if (transposeSparkMode) {
        spark.updateLEDs();
        return;
    }

    updateStepLEDs();
    updateStepUILEDs();
    updatePadLEDs();
    updateKnobLEDs();
    updateTrackButtonLEDs();
    updateTransportLEDs();
    updateCaptureLED();
    updateBackLED();
    updateOctaveLEDs();
}

function updateStepUILEDs() {
    /* Clear all step UI mode icons */
    setButtonLED(MoveStep1UI, Black);
    setButtonLED(MoveStep2UI, Black);
    setButtonLED(MoveStep3UI, Black);
    setButtonLED(MoveStep4UI, Black);
    /* Step 5 UI shows BPM mode option when shift held, or lit when in BPM mode */
    setButtonLED(MoveStep5UI, (state.shiftHeld || bpmEditMode) ? Cyan : Black);
    setButtonLED(MoveStep6UI, Black);
    setButtonLED(MoveStep7UI, Black);
    setButtonLED(MoveStep8UI, Black);
    setButtonLED(MoveStep9UI, Black);
    setButtonLED(MoveStep10UI, Black);
    setButtonLED(MoveStep11UI, Black);
    setButtonLED(MoveStep12UI, Black);
    setButtonLED(MoveStep13UI, Black);
    setButtonLED(MoveStep14UI, Black);
    setButtonLED(MoveStep15UI, Black);
    setButtonLED(MoveStep16UI, Black);
}

export function updateStepLEDs() {
    const currentPlayingStep = state.playing ? state.currentTransposeStep : -1;

    for (let i = 0; i < NUM_STEPS; i++) {
        const step = getTransposeStep(i);
        let color = Black;

        /* Spark preview mode - show which steps have sparks when Capture held */
        if (state.captureHeld && !transposeSparkMode) {
            if (step && (step.jump >= 0 || step.condition > 0)) {
                color = Purple;  /* Has spark settings */
            } else if (step) {
                color = LightGrey;  /* Has content but no spark */
            }
            /* Playhead overlay */
            if (i === currentPlayingStep) {
                color = White;
            }
        } else if (step) {
            /* Normal mode - step exists */
            if (i === state.heldTransposeStep) {
                color = White; /* Currently held */
            } else if (i === currentPlayingStep) {
                color = BrightGreen; /* Currently playing */
            } else if (step.jump >= 0 || step.condition > 0) {
                color = Cyan; /* Has content with spark */
            } else {
                color = Cyan; /* Has content */
            }
        } else if (i === state.heldTransposeStep) {
            color = DarkGrey; /* Held but empty */
        } else if (i === currentPlayingStep) {
            color = White; /* Playhead on empty step */
        }

        setLED(MoveSteps[i], color);
    }
}

function updatePadLEDs() {
    const detected = state.detectedScale;
    const heldStep = state.heldTransposeStep >= 0 ? getTransposeStep(state.heldTransposeStep) : null;
    const heldTranspose = heldStep ? heldStep.transpose : null;

    for (let i = 0; i < 32; i++) {
        const padNote = MovePads[i];

        /* Row 1 (top, indices 24-31): Chord follow with scroll offset */
        if (i >= 24) {
            const trackIdx = (i - 24) + state.trackScrollPosition;
            if (trackIdx >= NUM_TRACKS) {
                setLED(padNote, Black);
                continue;
            }
            const isFollowing = state.chordFollow[trackIdx];
            setLED(padNote, isFollowing ? TRACK_COLORS[trackIdx] : TRACK_COLORS_DIM[trackIdx]);
            continue;
        }

        /* Row 2 (indices 16-23): Reserved - black */
        if (i >= 16) {
            setLED(padNote, Black);
            continue;
        }

        /* Rows 3-4 (indices 0-15): Piano */
        const semitone = PAD_TO_SEMITONE[i];

        /* Gap pads */
        if (semitone === null) {
            setLED(padNote, Black);
            continue;
        }

        /* Calculate actual semitone with octave offset */
        const actualSemitone = semitone + (state.transposeOctaveOffset * 12);
        const pitchClass = ((semitone % 12) + 12) % 12; /* Handle negative */

        /* Determine color */
        let color;

        /* Calculate root offset for transpose comparison */
        const rootSemitone = detected ? detected.root : 0;

        /* Check if this is the held live transpose pad */
        if (state.heldLiveTransposePad === i) {
            color = BrightGreen; /* Bright green for active live transpose */
        }
        /* Check if this is the held step's transpose value */
        else if (heldTranspose !== null && actualSemitone === heldTranspose + rootSemitone) {
            color = TRACK_COLORS[state.currentTrack];
        } else if (detected && isRoot(pitchClass, detected)) {
            /* Root note */
            color = VividYellow;
        } else if (detected && isInScale(pitchClass, detected)) {
            /* In scale */
            color = WHITE_KEY_PADS.includes(i) ? White : LightGrey;
        } else {
            /* Out of scale or no detection - both white and black keys remain visible but dim */
            color = DarkGrey;
        }

        setLED(padNote, color);
    }
}

function updateKnobLEDs() {
    /* Knob 1 lit when holding step (duration control) */
    setButtonLED(MoveKnobLEDs[0], state.heldTransposeStep >= 0 ? Cyan : Black);

    /* Other knobs off */
    for (let i = 1; i < 8; i++) {
        setButtonLED(MoveKnobLEDs[i], Black);
    }
}

function updateTrackButtonLEDs() {
    /* Track buttons not used in master view - all off */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }
}

function updateTransportLEDs() {
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);
    setButtonLED(MoveLoop, state.sendClock ? Cyan : LightGrey);
    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
}

function updateCaptureLED() {
    /* Capture button - dim purple when held (consistent with track view) */
    setButtonLED(MoveCapture, state.captureHeld ? DarkPurple : Black);
}

function updateBackLED() {
    setButtonLED(MoveBack, White);
}

function updateOctaveLEDs() {
    /* Light up/down buttons based on available range */
    setButtonLED(MoveUp, state.transposeOctaveOffset < 2 ? White : DarkGrey);
    setButtonLED(MoveDown, state.transposeOctaveOffset > -2 ? White : DarkGrey);
}

/* ============ Display ============ */

export function updateDisplayContent() {
    /* Transpose spark mode display */
    if (transposeSparkMode) {
        spark.updateDisplayContent();
        return;
    }

    /* BPM edit mode display */
    if (bpmEditMode) {
        displayMessage(
            "BPM EDIT",
            `BPM: ${state.bpm}`,
            "Jog: adjust | Back: exit",
            ""
        );
        return;
    }

    const scaleName = getScaleDisplayName();
    const stepCount = getStepCount();
    const startTrack = state.trackScrollPosition + 1;
    const endTrack = state.trackScrollPosition + 8;
    const trackRange = `${startTrack}-${endTrack}`;

    let line2, line3;

    if (state.heldLiveTransposePad >= 0) {
        /* Live transpose mode - show current transpose value */
        const semitone = PAD_TO_SEMITONE[state.heldLiveTransposePad];
        const transpose = semitone + (state.transposeOctaveOffset * 12);
        const noteName = NOTE_NAMES[((transpose % 12) + 12) % 12];
        const octaveStr = transpose >= 0 ? `+${transpose}` : `${transpose}`;
        line2 = `LIVE TRANSPOSE: ${noteName} (${octaveStr})`;
        line3 = "Release pad to stop";
    } else if (state.heldTransposeStep >= 0) {
        const step = getTransposeStep(state.heldTransposeStep);
        if (step) {
            const noteName = NOTE_NAMES[((step.transpose % 12) + 12) % 12];
            const octaveStr = step.transpose >= 0 ? `+${step.transpose}` : `${step.transpose}`;
            line2 = `Step ${state.heldTransposeStep + 1}: ${noteName} (${octaveStr})`;
            line3 = `Duration: ${formatDuration(step.duration)}`;
        } else {
            line2 = `Step ${state.heldTransposeStep + 1}: (empty)`;
            line3 = "Press piano key to set";
        }
    } else {
        line2 = `Steps: ${stepCount}/${MAX_TRANSPOSE_STEPS}  T${trackRange}`;
        line3 = `Octave: ${state.transposeOctaveOffset >= 0 ? '+' : ''}${state.transposeOctaveOffset}`;
    }

    displayMessage(
        `Scale: ${scaleName}`,
        line2,
        line3,
        `${state.bpm} BPM  Sync: ${state.sendClock ? "ON" : "OFF"}`
    );
}

/* ============ Initialize delete held state ============ */
if (state.deleteHeld === undefined) {
    state.deleteHeld = false;
}
