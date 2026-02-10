/*
 * Master View - Transpose Spark Mode
 * Edit spark conditions (jump + condition) for selected transpose steps
 *
 * Simplified version of track spark mode:
 * - Only 2 parameters: jump target and condition
 * - No micro-timing offset (not applicable for transpose)
 * - Works with transposeSequence instead of pattern steps
 */

import {
    Black, White, LightGrey, Cyan, Purple, BrightGreen, BrightRed,
    MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8
} from "../../lib/shared-constants.js";

import { setLED, setButtonLED } from "../../lib/shared-input.js";
import { MoveKnobLEDs, CONDITIONS } from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import { syncTransposeSequenceToDSP } from '../../lib/helpers.js';
import { markDirty } from '../../lib/persistence.js';

const MAX_TRANSPOSE_STEPS = 16;

/* ============ Input Handling ============ */

/**
 * Handle input in transpose spark mode
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Step buttons - toggle selection */
    if (isNote && note >= 16 && note <= 31 && isNoteOn && velocity > 0) {
        const stepIdx = note - 16;
        toggleTransposeSparkStep(stepIdx);
        updateDisplayContent();
        updateLEDs();
        return true;
    }

    /* Knobs - adjust spark parameters */
    const knobs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];
    if (isCC && knobs.includes(note) && state.transposeSparkSelectedSteps.size > 0) {
        const knobIdx = knobs.indexOf(note);
        if (knobIdx === 0 || knobIdx === 7) {
            handleTransposeSparkKnob(knobIdx, velocity);
            return true;
        }
    }

    /* Ignore all other input - mode is focused */
    return true;
}

function toggleTransposeSparkStep(stepIdx) {
    if (state.transposeSparkSelectedSteps.has(stepIdx)) {
        state.transposeSparkSelectedSteps.delete(stepIdx);
    } else {
        state.transposeSparkSelectedSteps.add(stepIdx);
    }
}

function handleTransposeSparkKnob(knobIdx, velocity) {
    for (const stepIdx of state.transposeSparkSelectedSteps) {
        /* Only edit existing steps */
        if (stepIdx >= state.transposeSequence.length) continue;
        const step = state.transposeSequence[stepIdx];
        if (!step) continue;

        if (knobIdx === 0) {
            /* Jump - cycles from -1 to MAX_TRANSPOSE_STEPS-1 */
            let jump = step.jump !== undefined ? step.jump : -1;
            if (velocity >= 1 && velocity <= 63) {
                jump++;
                if (jump >= MAX_TRANSPOSE_STEPS) jump = -1;
            } else if (velocity >= 65 && velocity <= 127) {
                jump--;
                if (jump < -1) jump = MAX_TRANSPOSE_STEPS - 1;
            }
            step.jump = jump;
        } else if (knobIdx === 7) {
            /* Condition - cycles through CONDITIONS array */
            let condition = step.condition !== undefined ? step.condition : 0;
            if (velocity >= 1 && velocity <= 63) {
                condition = Math.min(condition + 1, CONDITIONS.length - 1);
            } else if (velocity >= 65 && velocity <= 127) {
                condition = Math.max(condition - 1, 0);
            }
            step.condition = condition;
        }
    }

    /* Sync all changes to DSP */
    syncTransposeSequenceToDSP();
    markDirty();
    updateDisplayContent();
    updateLEDs();
}

/* ============ LED Updates ============ */

/**
 * Update ALL LEDs for transpose spark mode
 * This mode owns all LEDs - nothing shared
 */
export function updateLEDs() {
    /* Steps - show spark state with playhead overlay */
    for (let i = 0; i < MAX_TRANSPOSE_STEPS; i++) {
        let color = Black;

        /* Steps with content */
        if (i < state.transposeSequence.length && state.transposeSequence[i]) {
            const step = state.transposeSequence[i];
            color = LightGrey;

            /* Steps with spark settings */
            if (step.jump >= 0 || step.condition > 0) {
                color = Cyan;
            }
        }

        /* Selected steps */
        if (state.transposeSparkSelectedSteps.has(i)) {
            color = Purple;
        }

        /* Playhead overlay */
        if (state.playing && i === state.currentTransposeStep) {
            color = White;
        }

        setLED(MoveSteps[i], color);
    }

    /* Knobs - show available controls */
    setButtonLED(MoveKnobLEDs[0], LightGrey);  // Jump
    for (let i = 1; i < 7; i++) {
        setButtonLED(MoveKnobLEDs[i], Black);
    }
    setButtonLED(MoveKnobLEDs[7], LightGrey);  // Condition

    /* Track buttons - all off (no track selection in this mode) */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }

    /* Transport - play/rec reflect global state */
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);
    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
    setButtonLED(MoveLoop, Black);

    /* Capture highlighted (we're in spark mode), Back lit for exit */
    setButtonLED(MoveCapture, Purple);
    setButtonLED(MoveBack, White);
}

/* ============ Playhead ============ */

/**
 * Get the color for a transpose step in spark mode
 */
function getStepColor(stepIdx) {
    /* Selected steps */
    if (state.transposeSparkSelectedSteps.has(stepIdx)) {
        return Purple;
    }

    /* Steps with content */
    if (stepIdx < state.transposeSequence.length && state.transposeSequence[stepIdx]) {
        const step = state.transposeSequence[stepIdx];

        /* Steps with spark settings */
        if (step.jump >= 0 || step.condition > 0) {
            return Cyan;
        }

        return LightGrey;
    }

    return Black;
}

/**
 * Lightweight playhead update - only updates the two step LEDs that changed
 */
export function updatePlayhead(oldStep, newStep) {
    /* Restore old step to its mode color */
    if (oldStep >= 0 && oldStep < MAX_TRANSPOSE_STEPS) {
        setLED(MoveSteps[oldStep], getStepColor(oldStep));
    }
    /* Set new step to playhead */
    if (newStep >= 0 && newStep < MAX_TRANSPOSE_STEPS) {
        setLED(MoveSteps[newStep], White);
    }
}

/* ============ Display ============ */

export function updateDisplayContent() {
    const selectedCount = state.transposeSparkSelectedSteps.size;
    if (selectedCount > 0) {
        const firstStep = [...state.transposeSparkSelectedSteps][0];
        const step = state.transposeSequence[firstStep];

        if (step) {
            const cond = CONDITIONS[step.condition || 0];
            const jumpStr = step.jump >= 0 ? `Step ${step.jump + 1}` : "None";
            displayMessage(
                `TRANSPOSE SPARK (${selectedCount})`,
                `Jump: ${jumpStr}`,
                `Cond: ${cond.name}`,
                ""
            );
        } else {
            displayMessage(
                `TRANSPOSE SPARK (${selectedCount})`,
                "Selected step is empty",
                "",
                ""
            );
        }
    } else {
        displayMessage("TRANSPOSE SPARK", "Select steps to edit", "", "");
    }
}

/* ============ Lifecycle ============ */

export function onEnter() {
    updateDisplayContent();
}

export function onExit() {
    state.transposeSparkSelectedSteps.clear();
}
