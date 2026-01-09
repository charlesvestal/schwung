/*
 * Track View - Spark Mode
 * Edit spark conditions (param spark, comp spark, jump) for selected steps
 *
 * This mode is fully self-contained:
 * - Owns ALL LEDs
 * - Handles step buttons for multi-select, knobs for params, jog for offset
 * - No track selection
 */

import {
    Black, LightGrey, Cyan, Purple, MoveMainKnob,
    MovePads, MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8
} from "../../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../../shared/input_filter.mjs";
import { NUM_STEPS, MoveKnobLEDs, CONDITIONS } from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import { setParam, getCurrentPattern } from '../../lib/helpers.js';

/* ============ Input Handling ============ */

/**
 * Handle input in spark mode
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
        toggleSparkStep(stepIdx);
        updateDisplayContent();
        updateLEDs();
        return true;
    }

    /* Knobs - adjust spark parameters */
    const knobs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];
    if (isCC && knobs.includes(note) && state.sparkSelectedSteps.size > 0) {
        const knobIdx = knobs.indexOf(note);
        if (knobIdx === 0 || knobIdx === 6 || knobIdx === 7) {
            handleSparkKnob(knobIdx, velocity);
            return true;
        }
    }

    /* Jog wheel - adjust offset for selected steps */
    if (isCC && note === MoveMainKnob && state.sparkSelectedSteps.size > 0) {
        handleSparkOffset(velocity);
        return true;
    }

    /* Ignore all other input - mode is focused */
    return true;
}

function toggleSparkStep(stepIdx) {
    if (state.sparkSelectedSteps.has(stepIdx)) {
        state.sparkSelectedSteps.delete(stepIdx);
    } else {
        state.sparkSelectedSteps.add(stepIdx);
    }
}

function handleSparkKnob(knobIdx, velocity) {
    const pattern = getCurrentPattern(state.currentTrack);

    for (const stepIdx of state.sparkSelectedSteps) {
        const step = pattern.steps[stepIdx];

        if (knobIdx === 0) {
            /* Jump */
            let jump = step.jump;
            if (velocity >= 1 && velocity <= 63) {
                jump = Math.min(jump + 1, NUM_STEPS - 1);
            } else if (velocity >= 65 && velocity <= 127) {
                jump = Math.max(jump - 1, -1);
            }
            step.jump = jump;
            setParam(`track_${state.currentTrack}_step_${stepIdx}_jump`, String(jump));
        } else if (knobIdx === 6) {
            /* Comp Spark */
            let compSpark = step.compSpark || 0;
            if (velocity >= 1 && velocity <= 63) {
                compSpark = Math.min(compSpark + 1, CONDITIONS.length - 1);
            } else if (velocity >= 65 && velocity <= 127) {
                compSpark = Math.max(compSpark - 1, 0);
            }
            step.compSpark = compSpark;
            const cond = CONDITIONS[compSpark];
            setParam(`track_${state.currentTrack}_step_${stepIdx}_comp_spark_n`, String(cond.n));
            setParam(`track_${state.currentTrack}_step_${stepIdx}_comp_spark_m`, String(cond.m));
            setParam(`track_${state.currentTrack}_step_${stepIdx}_comp_spark_not`, cond.not ? "1" : "0");
        } else if (knobIdx === 7) {
            /* Param Spark */
            let paramSpark = step.paramSpark || 0;
            if (velocity >= 1 && velocity <= 63) {
                paramSpark = Math.min(paramSpark + 1, CONDITIONS.length - 1);
            } else if (velocity >= 65 && velocity <= 127) {
                paramSpark = Math.max(paramSpark - 1, 0);
            }
            step.paramSpark = paramSpark;
            const cond = CONDITIONS[paramSpark];
            setParam(`track_${state.currentTrack}_step_${stepIdx}_param_spark_n`, String(cond.n));
            setParam(`track_${state.currentTrack}_step_${stepIdx}_param_spark_m`, String(cond.m));
            setParam(`track_${state.currentTrack}_step_${stepIdx}_param_spark_not`, cond.not ? "1" : "0");
        }
    }

    updateDisplayContent();
    updateLEDs();
}

function handleSparkOffset(velocity) {
    const pattern = getCurrentPattern(state.currentTrack);

    for (const stepIdx of state.sparkSelectedSteps) {
        const step = pattern.steps[stepIdx];
        let offset = step.offset || 0;

        if (velocity >= 1 && velocity <= 63) {
            offset = Math.min(offset + 1, 24);
        } else if (velocity >= 65 && velocity <= 127) {
            offset = Math.max(offset - 1, -24);
        }

        step.offset = offset;
        setParam(`track_${state.currentTrack}_step_${stepIdx}_offset`, String(offset));
    }

    const firstStep = [...state.sparkSelectedSteps][0];
    const offset = pattern.steps[firstStep].offset;
    const pct = Math.round((offset / 48) * 100);
    const sign = offset >= 0 ? "+" : "";
    displayMessage(
        `SPARK (${state.sparkSelectedSteps.size})`,
        `Offset: ${sign}${offset} ticks`,
        `(${sign}${pct}% of step)`,
        ""
    );
}

/* ============ LED Updates ============ */

/**
 * Update ALL LEDs for spark mode
 * This mode owns all LEDs - nothing shared
 */
export function updateLEDs() {
    const pattern = getCurrentPattern(state.currentTrack);

    /* Steps - show spark state */
    for (let i = 0; i < NUM_STEPS; i++) {
        let color = Black;
        const step = pattern.steps[i];

        /* Steps with content */
        if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
            color = LightGrey;
        }

        /* Steps with spark settings */
        if (step.paramSpark > 0 || step.compSpark > 0 || step.jump >= 0) {
            color = Cyan;
        }

        /* Selected steps */
        if (state.sparkSelectedSteps.has(i)) {
            color = Purple;
        }

        setLED(MoveSteps[i], color);
    }

    /* Pads - all off */
    for (let i = 0; i < 32; i++) {
        setLED(MovePads[i], Black);
    }

    /* Knobs - show available controls */
    setButtonLED(MoveKnobLEDs[0], LightGrey);  // Jump
    for (let i = 1; i < 6; i++) {
        setButtonLED(MoveKnobLEDs[i], Black);
    }
    setButtonLED(MoveKnobLEDs[6], LightGrey);  // Comp Spark
    setButtonLED(MoveKnobLEDs[7], LightGrey);  // Param Spark

    /* Track buttons - all off (no track selection in this mode) */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }

    /* Transport - play/rec reflect global state */
    setButtonLED(MovePlay, state.playing ? 127 : Black);
    setButtonLED(MoveRec, state.recording ? 127 : Black);
    setButtonLED(MoveLoop, Black);

    /* Capture highlighted (we're in spark mode), Back off */
    setButtonLED(MoveCapture, Purple);
    setButtonLED(MoveBack, Black);
}

/* ============ Display ============ */

export function updateDisplayContent() {
    const selectedCount = state.sparkSelectedSteps.size;
    if (selectedCount > 0) {
        const firstStep = [...state.sparkSelectedSteps][0];
        const step = getCurrentPattern(state.currentTrack).steps[firstStep];
        const paramCond = CONDITIONS[step.paramSpark || 0];
        const compCond = CONDITIONS[step.compSpark || 0];
        displayMessage(
            `SPARK MODE (${selectedCount})`,
            `Param: ${paramCond.name}`,
            `Comp: ${compCond.name}`,
            step.jump >= 0 ? `Jump: ${step.jump + 1}` : ""
        );
    } else {
        displayMessage("SPARK MODE", "No steps selected", "", "");
    }
}

/* ============ Lifecycle ============ */

export function onEnter() {
    updateDisplayContent();
}

export function onExit() {
    state.sparkSelectedSteps.clear();
}
