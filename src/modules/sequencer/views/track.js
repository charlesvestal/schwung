/*
 * Track View
 * Edit steps, notes, CC values, and step parameters
 * Sub-modes: normal, loop, spark, swing, bpm
 */

import {
    Black, White, LightGrey, Navy, Cyan, Purple, VividYellow,
    MoveSteps, MovePads, MoveLoop, MoveMainKnob, MoveMainButton, MoveCapture,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveKnob1Touch, MoveKnob2Touch, MoveKnob7Touch, MoveKnob8Touch
} from "../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../shared/input_filter.mjs";

import {
    NUM_TRACKS, NUM_STEPS, HOLD_THRESHOLD_MS,
    MoveKnobLEDs, TRACK_COLORS, TRACK_COLORS_DIM,
    SPEED_OPTIONS, RATCHET_VALUES, CONDITIONS
} from '../lib/constants.js';

import {
    state, displayMessage,
    enterLoopEdit, exitLoopEdit, enterSparkMode, exitSparkMode,
    enterSwingMode, exitSwingMode
} from '../lib/state.js';

import {
    setParam, getCurrentPattern, noteToName, notesToString,
    updateAndSendCC, updateTransportLEDs
} from '../lib/helpers.js';

/* ============ View Interface ============ */

/**
 * Called when entering track view
 */
export function onEnter() {
    updateDisplayContent();
}

/**
 * Called when exiting track view
 */
export function onExit() {
    state.heldStep = -1;
    state.loopEditFirst = -1;
    state.sparkSelectedSteps.clear();
}

/**
 * Handle MIDI input for track view
 * Returns true if handled, false to let router handle
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Handle knob touch for tap-to-clear */
    if (handleKnobTouch(data)) return true;

    /* Capture button - toggle spark mode */
    if (isCC && note === MoveCapture) {
        return handleCaptureButton(velocity);
    }

    /* Step buttons */
    if (isNote && note >= 16 && note <= 31) {
        return handleStepButton(note - 16, isNoteOn, velocity);
    }

    /* Pads */
    if (isNote && note >= 68 && note <= 99) {
        return handlePad(note - 68, isNoteOn, velocity);
    }

    /* Loop button */
    if (isCC && note === MoveLoop) {
        return handleLoopButton(velocity);
    }

    /* Knobs */
    const knobs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];
    if (isCC && knobs.includes(note)) {
        return handleKnob(knobs.indexOf(note), velocity);
    }

    /* Jog wheel click - exit swing mode */
    if (isNote && note === MoveMainButton && isNoteOn && velocity > 0) {
        if (state.trackMode === 'swing') {
            exitSwingMode();
            displayMessage("SEQOMD", `Track ${state.currentTrack + 1}`, "", "");
            updateStepLEDs();
            updateKnobLEDs();
            return true;
        }
    }

    /* Jog wheel turn */
    if (isCC && note === MoveMainKnob) {
        return handleJogWheel(velocity);
    }

    return false;  // Let router handle
}

/**
 * Update all LEDs for track view
 */
export function updateLEDs() {
    updateStepLEDs();
    updatePadLEDs();
    updateKnobLEDs();
}

/**
 * Update display content for track view
 */
export function updateDisplayContent() {
    if (state.shiftHeld) {
        const trackNum = state.currentTrack + 1;
        const ch = state.tracks[state.currentTrack].channel + 1;
        const speedName = SPEED_OPTIONS[state.tracks[state.currentTrack].speedIndex].name;

        displayMessage(
            `Track ${trackNum}`,
            `Channel: ${ch}`,
            `Speed: ${speedName}`,
            ""
        );
    } else {
        const trackNum = state.currentTrack + 1;
        const muteStr = state.tracks[state.currentTrack].muted ? " [MUTE]" : "";
        const pattern = getCurrentPattern(state.currentTrack);
        const loopStr = (pattern.loopStart > 0 || pattern.loopEnd < NUM_STEPS - 1)
            ? `Loop:${pattern.loopStart + 1}-${pattern.loopEnd + 1}`
            : "";

        displayMessage(
            `Track ${trackNum}${muteStr}`,
            `Pattern ${state.tracks[state.currentTrack].currentPattern + 1}`,
            loopStr,
            ""
        );
    }
}

/* ============ Input Handlers ============ */

function handleKnobTouch(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const note = data[1];
    const velocity = data[2];

    const touchKnobs = [MoveKnob1Touch, MoveKnob2Touch, MoveKnob7Touch, MoveKnob8Touch];
    const touchKnobIndices = [0, 1, 6, 7];

    if (isNote && touchKnobs.includes(note)) {
        const touchIdx = touchKnobs.indexOf(note);
        const knobIdx = touchKnobIndices[touchIdx];

        if (state.heldStep >= 0) {
            if (isNoteOn && velocity > 0) {
                state.knobTouchTime[knobIdx] = Date.now();
                state.knobTurned[knobIdx] = false;
            } else {
                if (state.knobTouchTime[knobIdx] && !state.knobTurned[knobIdx]) {
                    const touchDuration = Date.now() - state.knobTouchTime[knobIdx];
                    if (touchDuration < HOLD_THRESHOLD_MS) {
                        handleKnobTap(knobIdx);
                    }
                }
                delete state.knobTouchTime[knobIdx];
            }
        }
        return true;
    }
    return false;
}

function handleKnobTap(knobIdx) {
    const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];

    if (knobIdx === 0 && step.cc1 >= 0) {
        step.cc1 = -1;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_cc1`, "-1");
        displayMessage(undefined, `Step ${state.heldStep + 1}`, "CC1 cleared", "");
    } else if (knobIdx === 1 && step.cc2 >= 0) {
        step.cc2 = -1;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_cc2`, "-1");
        displayMessage(undefined, `Step ${state.heldStep + 1}`, "CC2 cleared", "");
    } else if (knobIdx === 6 && step.ratchet > 0) {
        step.ratchet = 0;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_ratchet`, "1");
        displayMessage(undefined, `Step ${state.heldStep + 1}`, "Ratchet: 1x", "");
    } else if (knobIdx === 7) {
        step.condition = 0;
        step.probability = 100;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_n`, "0");
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_m`, "0");
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_not`, "0");
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_probability`, "100");
        displayMessage(undefined, `Step ${state.heldStep + 1}`, "Cond/Prob cleared", "");
    }
    updateKnobLEDs();
    updateStepLEDs();
}

function handleCaptureButton(velocity) {
    if (velocity > 0) {
        if (state.trackMode === 'spark') {
            exitSparkMode();
            displayMessage("SEQOMD", `Track ${state.currentTrack + 1}`, "", "");
            updateStepLEDs();
        } else {
            state.captureHeld = true;
        }
    } else {
        state.captureHeld = false;
    }
    return true;
}

function handleStepButton(stepIdx, isNoteOn, velocity) {
    /* Swing mode: shift + step 7 enters swing edit mode */
    if (state.shiftHeld && stepIdx === 6 && isNoteOn && velocity > 0) {
        enterSwingMode();
        const swing = state.tracks[state.currentTrack].swing;
        displayMessage(
            "SWING MODE",
            `Track ${state.currentTrack + 1}`,
            `Swing: ${swing}%`,
            "Jog: adjust, Click: exit"
        );
        updateStepLEDs();
        updateKnobLEDs();
        return true;
    }

    /* Loop edit mode */
    if (state.trackMode === 'loop' && isNoteOn && velocity > 0) {
        return handleLoopEditStep(stepIdx);
    }

    /* Spark mode: capture + step or step toggle */
    if (state.captureHeld && isNoteOn && velocity > 0) {
        return handleSparkEntry(stepIdx);
    }

    if (state.trackMode === 'spark' && isNoteOn && velocity > 0) {
        return handleSparkStepToggle(stepIdx);
    }

    /* Normal step editing */
    return handleNormalStep(stepIdx, isNoteOn, velocity);
}

function handleLoopEditStep(stepIdx) {
    if (state.loopEditFirst < 0) {
        state.loopEditFirst = stepIdx;
        displayMessage(
            `Track ${state.currentTrack + 1} Loop`,
            `Start: ${stepIdx + 1}`,
            "Tap end step...",
            ""
        );
    } else {
        const startStep = Math.min(state.loopEditFirst, stepIdx);
        const endStep = Math.max(state.loopEditFirst, stepIdx);
        const pattern = getCurrentPattern(state.currentTrack);
        pattern.loopStart = startStep;
        pattern.loopEnd = endStep;
        setParam(`track_${state.currentTrack}_loop_start`, String(startStep));
        setParam(`track_${state.currentTrack}_loop_end`, String(endStep));
        displayMessage(
            `Track ${state.currentTrack + 1} Loop`,
            `Set: ${startStep + 1}-${endStep + 1}`,
            `${endStep - startStep + 1} steps`,
            ""
        );
        state.loopEditFirst = -1;
        updateTransportLEDs();
    }
    updateStepLEDs();
    return true;
}

function handleSparkEntry(stepIdx) {
    if (state.trackMode !== 'spark') {
        enterSparkMode();
    }
    toggleSparkStep(stepIdx);
    updateSparkDisplay();
    updateStepLEDs();
    updateKnobLEDs();
    return true;
}

function handleSparkStepToggle(stepIdx) {
    toggleSparkStep(stepIdx);
    updateSparkDisplay();
    updateStepLEDs();
    return true;
}

function toggleSparkStep(stepIdx) {
    if (state.sparkSelectedSteps.has(stepIdx)) {
        state.sparkSelectedSteps.delete(stepIdx);
    } else {
        state.sparkSelectedSteps.add(stepIdx);
    }
}

function updateSparkDisplay() {
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

function handleNormalStep(stepIdx, isNoteOn, velocity) {
    if (isNoteOn && velocity > 0) {
        /* Check if setting length */
        if (state.heldStep >= 0 && state.heldStep !== stepIdx && stepIdx > state.heldStep) {
            return handleStepLength(stepIdx);
        }

        /* Step pressed */
        setLED(MoveSteps[stepIdx], TRACK_COLORS[state.currentTrack]);
        state.stepPressTimes[stepIdx] = Date.now();
        state.stepPadPressed[stepIdx] = false;
        state.heldStep = stepIdx;
        updatePadLEDs();
        updateKnobLEDs();
        updateStepLEDs();
    } else {
        /* Step released */
        handleStepRelease(stepIdx);
    }
    return true;
}

function handleStepLength(stepIdx) {
    const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];
    const newLength = stepIdx - state.heldStep + 1;
    const currentLength = step.length || 1;

    if (currentLength === newLength) {
        step.length = 1;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_length`, "1");
        displayMessage(`Step ${state.heldStep + 1}`, `Length: 1 step`, "", "");
    } else {
        step.length = newLength;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_length`, String(newLength));
        displayMessage(`Step ${state.heldStep + 1}`, `Length: ${newLength} steps`, `→ Step ${stepIdx + 1}`, "");
    }
    state.stepPadPressed[state.heldStep] = true;
    updateStepLEDs();
    return true;
}

function handleStepRelease(stepIdx) {
    const pressTime = state.stepPressTimes[stepIdx];
    const padPressed = state.stepPadPressed[stepIdx];

    if (pressTime !== undefined && !padPressed) {
        const holdDuration = Date.now() - pressTime;
        if (holdDuration < HOLD_THRESHOLD_MS) {
            const step = getCurrentPattern(state.currentTrack).steps[stepIdx];
            if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
                clearStep(stepIdx);
            } else if (state.lastSelectedNote > 0) {
                toggleStepNote(stepIdx, state.lastSelectedNote);
            }
        }
    }

    delete state.stepPressTimes[stepIdx];
    delete state.stepPadPressed[stepIdx];

    if (state.heldStep === stepIdx) {
        state.heldStep = -1;
        updatePadLEDs();
        updateKnobLEDs();
    }
    updateStepLEDs();
}

function handlePad(padIdx, isNoteOn, velocity) {
    const midiNote = 36 + padIdx;

    if (state.heldStep >= 0) {
        /* Holding a step: toggle notes */
        if (isNoteOn && velocity > 0) {
            const wasAdded = toggleStepNote(state.heldStep, midiNote);
            state.stepPadPressed[state.heldStep] = true;
            setParam(`track_${state.currentTrack}_preview_note`, String(midiNote));

            const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];
            displayMessage(
                undefined,
                `Step ${state.heldStep + 1}`,
                `Notes: ${notesToString(step.notes)}`,
                wasAdded ? `Added ${noteToName(midiNote)}` : `Removed ${noteToName(midiNote)}`
            );
            updatePadLEDs();
            updateStepLEDs();
        } else {
            setParam(`track_${state.currentTrack}_preview_note_off`, String(midiNote));
        }
    } else {
        /* No step held: select note for quick entry */
        if (isNoteOn && velocity > 0) {
            state.lastSelectedNote = midiNote;
            state.heldPads.add(midiNote);
            setParam(`track_${state.currentTrack}_preview_note`, String(midiNote));
            setLED(MovePads[padIdx], TRACK_COLORS[state.currentTrack]);

            /* Recording mode */
            if (state.recording && state.playing && state.currentPlayStep >= 0) {
                const step = getCurrentPattern(state.currentTrack).steps[state.currentPlayStep];
                if (!step.notes.includes(midiNote) && step.notes.length < 4) {
                    step.notes.push(midiNote);
                    setParam(`track_${state.currentTrack}_step_${state.currentPlayStep}_add_note`, String(midiNote));
                }
                state.lastRecordedStep = state.currentPlayStep;
            }
        } else {
            state.heldPads.delete(midiNote);
            setParam(`track_${state.currentTrack}_preview_note_off`, String(midiNote));
            setLED(MovePads[padIdx], Black);
        }
    }
    return true;
}

function handleLoopButton(velocity) {
    if (velocity > 0) {
        enterLoopEdit();
        const pattern = getCurrentPattern(state.currentTrack);
        displayMessage(
            `Track ${state.currentTrack + 1} Loop`,
            `Current: ${pattern.loopStart + 1}-${pattern.loopEnd + 1}`,
            "Tap start step...",
            ""
        );
        updateStepLEDs();
    } else {
        if (state.trackMode === 'loop') {
            exitLoopEdit();
            updateDisplayContent();
            updateStepLEDs();
            updateTransportLEDs();
        }
    }
    return true;
}

function handleKnob(knobIdx, velocity) {
    state.knobTurned[knobIdx] = true;

    if (state.shiftHeld) {
        return handleShiftKnob(knobIdx, velocity);
    }

    if (state.trackMode === 'spark' && state.sparkSelectedSteps.size > 0) {
        return handleSparkKnob(knobIdx, velocity);
    }

    if (state.heldStep >= 0 && (knobIdx < 2 || knobIdx === 6 || knobIdx === 7)) {
        return handleStepKnob(knobIdx, velocity);
    }

    /* Track mode: knobs 1-2 send CCs */
    if (knobIdx < 2) {
        const cc = 20 + (state.currentTrack * 2) + knobIdx;
        const ccValIdx = state.currentTrack * 2 + knobIdx;
        const channel = state.tracks[state.currentTrack].channel;
        const val = updateAndSendCC(state.trackCCValues, ccValIdx, velocity, cc, channel);
        displayMessage(
            `Track ${state.currentTrack + 1}`,
            `Knob ${knobIdx + 1}: CC ${cc}`,
            `Value: ${val}  Ch: ${channel + 1}`,
            ""
        );
        return true;
    }

    return false;
}

function handleShiftKnob(knobIdx, velocity) {
    if (knobIdx === 6) {
        /* Speed */
        let speedIdx = state.tracks[state.currentTrack].speedIndex;
        if (velocity >= 1 && velocity <= 63) {
            speedIdx = Math.min(speedIdx + 1, SPEED_OPTIONS.length - 1);
        } else if (velocity >= 65 && velocity <= 127) {
            speedIdx = Math.max(speedIdx - 1, 0);
        }
        state.tracks[state.currentTrack].speedIndex = speedIdx;
        setParam(`track_${state.currentTrack}_speed`, String(SPEED_OPTIONS[speedIdx].mult));
        updateDisplayContent();
        return true;
    } else if (knobIdx === 7) {
        /* Channel */
        let channel = state.tracks[state.currentTrack].channel;
        if (velocity >= 1 && velocity <= 63) {
            channel = (channel + 1) % 16;
        } else if (velocity >= 65 && velocity <= 127) {
            channel = (channel - 1 + 16) % 16;
        }
        state.tracks[state.currentTrack].channel = channel;
        setParam(`track_${state.currentTrack}_channel`, String(channel));
        updateDisplayContent();
        return true;
    }
    return false;
}

function handleSparkKnob(knobIdx, velocity) {
    if (knobIdx !== 0 && knobIdx !== 6 && knobIdx !== 7) return false;

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

    updateSparkDisplay();
    updateStepLEDs();
    return true;
}

function handleStepKnob(knobIdx, velocity) {
    const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];

    if (knobIdx < 2) {
        /* CC values */
        let val = knobIdx === 0 ? step.cc1 : step.cc2;
        if (val < 0) val = 64;

        if (velocity >= 1 && velocity <= 63) {
            val = Math.min(val + 1, 127);
        } else if (velocity >= 65 && velocity <= 127) {
            val = Math.max(val - 1, 0);
        }

        if (knobIdx === 0) {
            step.cc1 = val;
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_cc1`, String(val));
        } else {
            step.cc2 = val;
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_cc2`, String(val));
        }

        const cc = 20 + (state.currentTrack * 2) + knobIdx;
        displayMessage(
            `Step ${state.heldStep + 1}`,
            `CC${knobIdx + 1}: ${val}`,
            `(CC ${cc} on Ch ${state.tracks[state.currentTrack].channel + 1})`,
            ""
        );
    } else if (knobIdx === 6) {
        /* Ratchet */
        let ratchIdx = step.ratchet;
        if (velocity >= 1 && velocity <= 63) {
            ratchIdx = Math.min(ratchIdx + 1, RATCHET_VALUES.length - 1);
        } else if (velocity >= 65 && velocity <= 127) {
            ratchIdx = Math.max(ratchIdx - 1, 0);
        }
        step.ratchet = ratchIdx;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_ratchet`, String(RATCHET_VALUES[ratchIdx]));
        displayMessage(`Step ${state.heldStep + 1}`, `Ratchet: ${RATCHET_VALUES[ratchIdx]}x`, "", "");
    } else if (knobIdx === 7) {
        /* Probability / Condition */
        if (velocity >= 65 && velocity <= 127) {
            step.condition = 0;
            step.probability = Math.max(step.probability - 5, 5);
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_probability`, String(step.probability));
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_n`, "0");
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_m`, "0");
            displayMessage(`Step ${state.heldStep + 1}`, `Probability: ${step.probability}%`, "", "");
        } else if (velocity >= 1 && velocity <= 63) {
            step.probability = 100;
            step.condition = Math.min(step.condition + 1, CONDITIONS.length - 1);
            const cond = CONDITIONS[step.condition];
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_probability`, "100");
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_n`, String(cond.n));
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_m`, String(cond.m));
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_not`, cond.not ? "1" : "0");

            let desc = "Always";
            if (step.condition > 0) {
                desc = cond.not ? `Skip loop ${cond.m} of ${cond.n}` : `Play on loop ${cond.m} of ${cond.n}`;
            }
            displayMessage(`Step ${state.heldStep + 1}`, `Condition: ${cond.name}`, desc, "");
        }
    }

    updateKnobLEDs();
    return true;
}

function handleJogWheel(velocity) {
    if (state.trackMode === 'swing') {
        let swing = state.tracks[state.currentTrack].swing;
        if (velocity >= 1 && velocity <= 63) {
            swing = Math.min(swing + 1, 100);
        } else if (velocity >= 65 && velocity <= 127) {
            swing = Math.max(swing - 1, 0);
        }
        state.tracks[state.currentTrack].swing = swing;
        setParam(`track_${state.currentTrack}_swing`, String(swing));
        displayMessage(
            "SWING MODE",
            `Track ${state.currentTrack + 1}`,
            `Swing: ${swing}%`,
            "Jog: adjust, Click: exit"
        );
        return true;
    }

    if (state.shiftHeld) {
        /* BPM control */
        if (velocity >= 1 && velocity <= 63) {
            state.bpm = Math.min(state.bpm + 1, 300);
        } else if (velocity >= 65 && velocity <= 127) {
            state.bpm = Math.max(state.bpm - 1, 20);
        }
        setParam("bpm", String(state.bpm));
        displayMessage("BPM", `${state.bpm}`, "", "");
        return true;
    }

    return false;
}

/* ============ Step Helpers ============ */

function toggleStepNote(stepIdx, note) {
    const step = getCurrentPattern(state.currentTrack).steps[stepIdx];
    const noteIdx = step.notes.indexOf(note);

    if (noteIdx >= 0) {
        step.notes.splice(noteIdx, 1);
        setParam(`track_${state.currentTrack}_step_${stepIdx}_remove_note`, String(note));
        return false;
    } else {
        if (step.notes.length < 4) {
            step.notes.push(note);
            setParam(`track_${state.currentTrack}_step_${stepIdx}_add_note`, String(note));
            return true;
        }
        return false;
    }
}

function clearStep(stepIdx) {
    const step = getCurrentPattern(state.currentTrack).steps[stepIdx];
    step.notes = [];
    step.cc1 = -1;
    step.cc2 = -1;
    step.probability = 100;
    step.condition = 0;
    step.ratchet = 0;
    step.paramSpark = 0;
    step.compSpark = 0;
    step.jump = -1;
    setParam(`track_${state.currentTrack}_step_${stepIdx}_clear`, "1");
}

/* ============ LED Updates ============ */

function updateStepLEDs() {
    const pattern = getCurrentPattern(state.currentTrack);
    const trackColor = TRACK_COLORS[state.currentTrack];
    const dimColor = TRACK_COLORS_DIM[state.currentTrack];

    if (state.trackMode === 'loop') {
        for (let i = 0; i < NUM_STEPS; i++) {
            let color = Black;
            if (i >= pattern.loopStart && i <= pattern.loopEnd) {
                color = LightGrey;
            }
            if (i === pattern.loopStart || i === pattern.loopEnd) {
                color = trackColor;
            }
            if (state.loopEditFirst >= 0 && i === state.loopEditFirst) {
                color = Cyan;
            }
            setLED(MoveSteps[i], color);
        }
        return;
    }

    if (state.trackMode === 'spark') {
        for (let i = 0; i < NUM_STEPS; i++) {
            let color = Black;
            const step = pattern.steps[i];
            if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
                color = LightGrey;
            }
            if (step.paramSpark > 0 || step.compSpark > 0 || step.jump >= 0) {
                color = Cyan;
            }
            if (state.sparkSelectedSteps.has(i)) {
                color = Purple;
            }
            setLED(MoveSteps[i], color);
        }
        return;
    }

    if (state.trackMode === 'swing') {
        for (let i = 0; i < NUM_STEPS; i++) {
            setLED(MoveSteps[i], i === 6 ? VividYellow : Black);
        }
        return;
    }

    /* Normal mode */
    for (let i = 0; i < NUM_STEPS; i++) {
        let color = Black;
        const inLoop = i >= pattern.loopStart && i <= pattern.loopEnd;
        const step = pattern.steps[i];

        if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
            color = inLoop ? dimColor : Navy;
        }

        if (state.heldStep >= 0 && state.heldStep !== i) {
            const heldStepData = pattern.steps[state.heldStep];
            const heldLength = heldStepData.length || 1;
            const lengthEnd = state.heldStep + heldLength - 1;
            if (i > state.heldStep && i <= lengthEnd) {
                color = Cyan;
            }
        }

        if (state.playing && i === state.currentPlayStep && state.heldStep < 0) {
            color = White;
        }

        if (i === state.heldStep) {
            color = trackColor;
        }

        if (state.shiftHeld && i === 6) {
            color = VividYellow;
        }

        setLED(MoveSteps[i], color);
    }
}

function updatePadLEDs() {
    if (state.heldStep >= 0) {
        const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];
        const stepNotes = step.notes;
        const trackColor = TRACK_COLORS[state.currentTrack];

        for (let i = 0; i < 32; i++) {
            const midiNote = 36 + i;
            setLED(MovePads[i], stepNotes.includes(midiNote) ? trackColor : Black);
        }
    } else {
        for (let i = 0; i < 32; i++) {
            setLED(MovePads[i], Black);
        }
    }
}

function updateKnobLEDs() {
    const trackColor = TRACK_COLORS[state.currentTrack];

    if (state.shiftHeld) {
        for (let i = 0; i < 6; i++) {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
        setButtonLED(MoveKnobLEDs[6], VividYellow);
        setButtonLED(MoveKnobLEDs[7], Cyan);
        return;
    }

    if (state.trackMode === 'spark') {
        setButtonLED(MoveKnobLEDs[0], LightGrey);
        for (let i = 1; i < 6; i++) {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
        setButtonLED(MoveKnobLEDs[6], LightGrey);
        setButtonLED(MoveKnobLEDs[7], LightGrey);
        return;
    }

    if (state.trackMode === 'swing') {
        for (let i = 0; i < 8; i++) {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
        return;
    }

    if (state.heldStep >= 0) {
        const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];
        setButtonLED(MoveKnobLEDs[0], step.cc1 >= 0 ? trackColor : LightGrey);
        setButtonLED(MoveKnobLEDs[1], step.cc2 >= 0 ? trackColor : LightGrey);
        for (let i = 2; i < 6; i++) {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
        setButtonLED(MoveKnobLEDs[6], step.ratchet > 0 ? trackColor : LightGrey);
        const hasProb = step.probability < 100;
        const hasCond = step.condition > 0;
        setButtonLED(MoveKnobLEDs[7], (hasProb || hasCond) ? trackColor : LightGrey);
        return;
    }

    /* Default track mode */
    setButtonLED(MoveKnobLEDs[0], trackColor);
    setButtonLED(MoveKnobLEDs[1], trackColor);
    for (let i = 2; i < 8; i++) {
        setButtonLED(MoveKnobLEDs[i], Black);
    }
}
