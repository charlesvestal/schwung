/*
 * Track View - Normal Mode
 * Main step editing mode for notes, CCs, and step parameters
 *
 * This mode is fully self-contained:
 * - Owns ALL LEDs
 * - Handles all input: steps, pads, knobs, track buttons, jog wheel
 * - Track selection happens here
 */

import {
    Black, White, Navy, LightGrey, DarkGrey, Cyan, VividYellow, BrightGreen, BrightRed, Purple, DarkPurple,
    MoveMainKnob, MovePads, MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack, MoveUp, MoveDown,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveKnob1Touch, MoveKnob2Touch, MoveKnob3Touch, MoveKnob7Touch, MoveKnob8Touch,
    MoveStep1UI, MoveStep2UI, MoveStep5UI, MoveStep7UI, MoveStep8UI, MoveStep11UI
} from "../../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../../shared/input_filter.mjs";

import {
    NUM_TRACKS, NUM_STEPS, HOLD_THRESHOLD_MS, COPY_HOLD_MS, DISPLAY_RETURN_MS, MoveKnobLEDs,
    TRACK_COLORS, TRACK_COLORS_DIM, SPEED_OPTIONS, RATCHET_VALUES, CONDITIONS,
    ARP_MODES, ARP_SPEEDS, ARP_OCTAVES, ARP_LAYERS,
    getRatchetDisplayName
} from '../../lib/constants.js';

import { state, displayMessage } from '../../lib/state.js';

import {
    setParam, getCurrentPattern, noteToName, notesToString, updateAndSendCC, getPadBaseColor, updatePadLEDs
} from '../../lib/helpers.js';

import { detectScale } from '../../lib/scale_detection.js';
import { markDirty } from '../../lib/persistence.js';
import { cloneStep } from '../../lib/data.js';

/* ============ Display Return Helper ============ */

/**
 * Schedule display to return to default content after a delay.
 * Called when changing step parameters while holding a step.
 */
function scheduleDisplayReturn() {
    state.displayReturnTime = Date.now() + DISPLAY_RETURN_MS;
}

/**
 * Check if display should return to default and do so.
 * Called from tick().
 */
export function checkDisplayReturn() {
    if (state.displayReturnTime > 0 && Date.now() > state.displayReturnTime) {
        state.displayReturnTime = 0;
        updateDisplayContent();
    }
}

/**
 * Check if any held step has reached copy threshold and copy it.
 * Called from tick().
 */
export function checkCopyHold() {
    if (state.heldStep >= 0) {
        const stepIdx = state.heldStep;
        const pressTime = state.stepPressTimes[stepIdx];

        if (pressTime !== undefined && !state.copyHoldDetected[stepIdx]) {
            const holdDuration = Date.now() - pressTime;

            if (holdDuration >= COPY_HOLD_MS) {
                const step = getCurrentPattern(state.currentTrack).steps[stepIdx];
                const hasContent = step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0;

                if (hasContent) {
                    // Copy step to buffer
                    state.stepCopyBuffer = cloneStep(step);
                    state.copiedStepIdx = stepIdx;
                    state.copyHoldDetected[stepIdx] = true;

                    displayMessage(
                        undefined,
                        `Copied Step ${stepIdx + 1}`,
                        "Tap empty step to paste",
                        ""
                    );
                    scheduleDisplayReturn();
                }
            }
        }
    }
}

/* ============ Input Handling ============ */

/**
 * Handle input in normal mode
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Knob touch for tap-to-clear */
    if (handleKnobTouch(data)) return true;

    /* Track buttons - select track */
    if (isCC && MoveTracks.includes(note)) {
        return handleTrackButton(note, velocity);
    }

    /* Step buttons */
    if (isNote && note >= 16 && note <= 31) {
        return handleStepButton(note - 16, isNoteOn, velocity);
    }

    /* Pads */
    if (isNote && note >= 68 && note <= 99) {
        return handlePad(note - 68, isNoteOn, velocity);
    }

    /* Knobs */
    const knobs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];
    if (isCC && knobs.includes(note)) {
        return handleKnob(knobs.indexOf(note), velocity);
    }

    /* Jog wheel */
    if (isCC && note === MoveMainKnob) {
        return handleJogWheel(velocity);
    }

    /* Up/Down - octave shift for pads */
    if (isCC && note === MoveUp && velocity > 0) {
        if (state.padOctaveOffset < 2) {
            state.padOctaveOffset++;
            displayMessage("Pad Octave", `Offset: ${state.padOctaveOffset >= 0 ? '+' : ''}${state.padOctaveOffset}`, "", "");
            updatePadLEDs();
            updateOctaveLEDs();
        }
        return true;
    }
    if (isCC && note === MoveDown && velocity > 0) {
        if (state.padOctaveOffset > -2) {
            state.padOctaveOffset--;
            displayMessage("Pad Octave", `Offset: ${state.padOctaveOffset >= 0 ? '+' : ''}${state.padOctaveOffset}`, "", "");
            updatePadLEDs();
            updateOctaveLEDs();
        }
        return true;
    }

    return false;
}

/* ============ Track Button Handling ============ */

/**
 * Get track index for a track button position
 * All 4 buttons now show consecutive tracks from scroll position
 * Button 0 (top, CC 43): trackScrollPosition + 0
 * Button 1: trackScrollPosition + 1
 * Button 2: trackScrollPosition + 2
 * Button 3 (bottom): trackScrollPosition + 3
 */
function getTrackAtButton(btnPosition) {
    const trackIdx = state.trackScrollPosition + btnPosition;
    if (trackIdx >= 0 && trackIdx < NUM_TRACKS) {
        return trackIdx;
    }
    return -1;  // Out of range
}

function handleTrackButton(note, velocity) {
    if (velocity === 0) return true;

    const btnIdx = MoveTracks.indexOf(note);
    const btnPosition = 3 - btnIdx;  /* Reverse: top = 0, bottom = 3 */

    const trackIdx = getTrackAtButton(btnPosition);
    if (trackIdx >= 0 && trackIdx < NUM_TRACKS) {
        state.currentTrack = trackIdx;
        /* Auto-scroll to ensure selected track is visible in 4-button window */
        const group = Math.floor(trackIdx / 4) * 4;  // 0, 4, 8, or 12
        state.trackScrollPosition = group;
        updateDisplayContent();
        updateLEDs();
    }

    return true;
}

/* ============ Knob Touch Handling ============ */

function handleKnobTouch(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const note = data[1];
    const velocity = data[2];

    const touchKnobs = [MoveKnob1Touch, MoveKnob2Touch, MoveKnob3Touch, MoveKnob7Touch, MoveKnob8Touch];
    const touchKnobIndices = [0, 1, 2, 6, 7];

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
    let changed = false;

    if (knobIdx === 0 && step.cc1 >= 0) {
        step.cc1 = -1;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_cc1`, "-1");
        displayMessage(undefined, `Step ${state.heldStep + 1}`, "CC1 cleared", "");
        scheduleDisplayReturn();
        changed = true;
    } else if (knobIdx === 1 && step.cc2 >= 0) {
        step.cc2 = -1;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_cc2`, "-1");
        displayMessage(undefined, `Step ${state.heldStep + 1}`, "CC2 cleared", "");
        scheduleDisplayReturn();
        changed = true;
    } else if (knobIdx === 2) {
        /* Cycle step arp parameter: mode -> speed -> octave -> layer -> mode */
        state.stepArpParam = (state.stepArpParam + 1) % 4;
        displayStepArpParams();
        scheduleDisplayReturn();
    } else if (knobIdx === 6 && step.ratchet > 0) {
        step.ratchet = 0;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_ratchet`, "1");
        displayMessage(undefined, `Step ${state.heldStep + 1}`, getRatchetDisplayName(1), "");
        scheduleDisplayReturn();
        changed = true;
    } else if (knobIdx === 7) {
        step.condition = 0;
        step.probability = 100;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_n`, "0");
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_m`, "0");
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_condition_not`, "0");
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_probability`, "100");
        displayMessage(undefined, `Step ${state.heldStep + 1}`, "Cond/Prob cleared", "");
        scheduleDisplayReturn();
        changed = true;
    }
    if (changed) markDirty();
    updateLEDs();
}

/* ============ Step Button Handling ============ */

function handleStepButton(stepIdx, isNoteOn, velocity) {
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
        updateLEDs();
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
        displayMessage(`Step ${state.heldStep + 1}`, `Length: ${newLength} steps`, `-> Step ${stepIdx + 1}`, "");
    }
    scheduleDisplayReturn();
    state.stepPadPressed[state.heldStep] = true;
    markDirty();
    updateLEDs();
    return true;
}

function handleStepRelease(stepIdx) {
    const pressTime = state.stepPressTimes[stepIdx];
    const padPressed = state.stepPadPressed[stepIdx];
    let changed = false;

    if (pressTime !== undefined && !padPressed) {
        const holdDuration = Date.now() - pressTime;
        if (holdDuration < HOLD_THRESHOLD_MS) {
            const step = getCurrentPattern(state.currentTrack).steps[stepIdx];
            const hasContent = step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0;

            // Paste if buffer exists and step is empty
            if (state.stepCopyBuffer && !hasContent) {
                pasteStep(stepIdx);
                changed = true;
            }
            // Clear if has content (existing behavior)
            else if (hasContent) {
                clearStep(stepIdx);
                changed = true;
            }
            // Add note if empty and note selected (existing behavior)
            else if (state.lastSelectedNote > 0) {
                toggleStepNote(stepIdx, state.lastSelectedNote, state.lastSelectedVelocity);
                changed = true;
            }
        }
    }

    delete state.stepPressTimes[stepIdx];
    delete state.stepPadPressed[stepIdx];
    delete state.copyHoldDetected[stepIdx];

    if (state.heldStep === stepIdx) {
        state.heldStep = -1;
        state.stepArpParam = 0;  /* Reset arp param selection for next step */
        state.displayReturnTime = 0;  /* Clear any pending display return */
    }
    if (changed) markDirty();
    updateLEDs();
}

/* ============ Pad Handling ============ */

function handlePad(padIdx, isNoteOn, velocity) {
    // Cancel copy buffer on any pad press
    if (isNoteOn && velocity > 0 && state.stepCopyBuffer) {
        state.stepCopyBuffer = null;
        state.copiedStepIdx = -1;
    }

    const baseNote = 36 + padIdx + (state.padOctaveOffset * 12);
    const midiNote = Math.max(0, Math.min(127, baseNote));

    if (state.heldStep >= 0) {
        /* Holding a step: toggle notes */
        if (isNoteOn && velocity > 0) {
            const wasAdded = toggleStepNote(state.heldStep, midiNote, velocity);
            state.stepPadPressed[state.heldStep] = true;
            setParam(`track_${state.currentTrack}_preview_velocity`, String(velocity));
            setParam(`track_${state.currentTrack}_preview_note`, String(midiNote));
            markDirty();

            const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];
            displayMessage(
                undefined,
                `Step ${state.heldStep + 1}`,
                `Notes: ${notesToString(step.notes)}`,
                wasAdded ? `Added ${noteToName(midiNote)} (vel ${step.velocity})` : `Removed ${noteToName(midiNote)}`
            );
            scheduleDisplayReturn();
            updateLEDs();
        } else {
            setParam(`track_${state.currentTrack}_preview_note_off`, String(midiNote));
        }
    } else {
        /* No step held: select note for quick entry */
        if (isNoteOn && velocity > 0) {
            state.lastSelectedNote = midiNote;
            state.lastSelectedVelocity = velocity;
            state.heldPads.add(midiNote);
            setParam(`track_${state.currentTrack}_preview_velocity`, String(velocity));
            setParam(`track_${state.currentTrack}_preview_note`, String(midiNote));
            setLED(MovePads[padIdx], TRACK_COLORS[state.currentTrack]);

            /* Recording mode */
            if (state.recording && state.playing && state.currentPlayStep >= 0) {
                const step = getCurrentPattern(state.currentTrack).steps[state.currentPlayStep];
                if (!step.notes.includes(midiNote) && step.notes.length < 7) {
                    step.notes.push(midiNote);
                    /* Capture velocity from pad press */
                    step.velocity = Math.max(1, Math.min(127, velocity));
                    setParam(`track_${state.currentTrack}_step_${state.currentPlayStep}_add_note`, String(midiNote));
                    setParam(`track_${state.currentTrack}_step_${state.currentPlayStep}_velocity`, String(step.velocity));
                    markDirty();
                }
                state.lastRecordedStep = state.currentPlayStep;
            }
        } else {
            state.heldPads.delete(midiNote);
            setParam(`track_${state.currentTrack}_preview_note_off`, String(midiNote));
            /* Restore to base color instead of black */
            setLED(MovePads[padIdx], getPadBaseColor(padIdx));
        }
    }
    return true;
}

/* ============ Knob Handling ============ */

function handleKnob(knobIdx, velocity) {
    state.knobTurned[knobIdx] = true;

    if (state.shiftHeld) {
        return handleShiftKnob(knobIdx, velocity);
    }

    if (state.heldStep >= 0 && (knobIdx < 3 || knobIdx === 6 || knobIdx === 7)) {
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
        markDirty();
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
        markDirty();
        updateDisplayContent();
        return true;
    }
    return false;
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
    } else if (knobIdx === 2) {
        /* Step arp - single knob controls mode/speed/octave based on stepArpParam */
        if (state.stepArpParam === 0) {
            /* Mode */
            let mode = step.arpMode;
            if (velocity >= 1 && velocity <= 63) {
                mode = Math.min(mode + 1, ARP_MODES.length - 1);
            } else if (velocity >= 65 && velocity <= 127) {
                mode = Math.max(mode - 1, -1);
            }
            step.arpMode = mode;
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_arp_mode`, String(mode));
        } else if (state.stepArpParam === 1) {
            /* Speed */
            let speed = step.arpSpeed;
            if (velocity >= 1 && velocity <= 63) {
                speed = Math.min(speed + 1, ARP_SPEEDS.length - 1);
            } else if (velocity >= 65 && velocity <= 127) {
                speed = Math.max(speed - 1, -1);
            }
            step.arpSpeed = speed;
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_arp_speed`, String(speed));
        } else if (state.stepArpParam === 2) {
            /* Octave */
            let octave = step.arpOctave;
            if (velocity >= 1 && velocity <= 63) {
                octave = Math.min(octave + 1, ARP_OCTAVES.length - 1);
            } else if (velocity >= 65 && velocity <= 127) {
                octave = Math.max(octave - 1, -1);
            }
            step.arpOctave = octave;
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_arp_octave`, String(octave));
        } else {
            /* Layer - no track default, always 0-2 */
            let layer = step.arpLayer;
            if (velocity >= 1 && velocity <= 63) {
                layer = Math.min(layer + 1, ARP_LAYERS.length - 1);
            } else if (velocity >= 65 && velocity <= 127) {
                layer = Math.max(layer - 1, 0);
            }
            step.arpLayer = layer;
            setParam(`track_${state.currentTrack}_step_${state.heldStep}_arp_layer`, String(layer));
        }
        displayStepArpParams();
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
        displayMessage(`Step ${state.heldStep + 1}`, getRatchetDisplayName(RATCHET_VALUES[ratchIdx]), "", "");
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

    scheduleDisplayReturn();
    markDirty();
    updateLEDs();
    return true;
}

/* ============ Step Arp Display ============ */

const STEP_ARP_COLORS = [Cyan, VividYellow, Purple, BrightGreen];  /* Mode, Speed, Octave, Layer */

function displayStepArpParams() {
    const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];

    const modeName = step.arpMode < 0 ? "Track" : ARP_MODES[step.arpMode].name;
    const speedName = step.arpSpeed < 0 ? "Track" : ARP_SPEEDS[step.arpSpeed].name;
    const octaveName = step.arpOctave < 0 ? "Track" : ARP_OCTAVES[step.arpOctave].name;
    const layerName = ARP_LAYERS[step.arpLayer].name;  /* No track default for layer */

    /* Show one parameter per line for better readability */
    const lines = [
        `Mode${state.stepArpParam === 0 ? ">" : ""}: ${modeName}`,
        `Speed${state.stepArpParam === 1 ? ">" : ""}: ${speedName}`,
        `Octave${state.stepArpParam === 2 ? ">" : ""}: ${octaveName}`,
        `Layer${state.stepArpParam === 3 ? ">" : ""}: ${layerName}`
    ];

    displayMessage(lines[0], lines[1], lines[2], lines[3]);
    updateLEDs();
}

/* ============ Jog Wheel Handling ============ */

function handleJogWheel(velocity) {
    /* Micro-timing offset when holding step */
    if (state.heldStep >= 0) {
        const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];
        let offset = step.offset || 0;

        if (velocity >= 1 && velocity <= 63) {
            offset = Math.min(offset + 1, 24);
        } else if (velocity >= 65 && velocity <= 127) {
            offset = Math.max(offset - 1, -24);
        }

        step.offset = offset;
        setParam(`track_${state.currentTrack}_step_${state.heldStep}_offset`, String(offset));
        markDirty();

        /* Display offset as percentage of step */
        const pct = Math.round((offset / 48) * 100);
        const sign = offset >= 0 ? "+" : "";
        displayMessage(
            `Step ${state.heldStep + 1}`,
            `Offset: ${sign}${offset} ticks`,
            `(${sign}${pct}% of step)`,
            offset === 0 ? "On grid" : (offset < 0 ? "Early" : "Late")
        );
        scheduleDisplayReturn();
        state.stepPadPressed[state.heldStep] = true;
        return true;
    }

    /* Shift + jog: scroll patterns vertically */
    if (state.shiftHeld) {
        if (velocity >= 1 && velocity <= 63) {
            state.patternViewOffset = Math.min(state.patternViewOffset + 1, NUM_TRACKS - 4);
        } else if (velocity >= 65 && velocity <= 127) {
            state.patternViewOffset = Math.max(state.patternViewOffset - 1, 0);
        }
        displayMessage("Pattern Scroll", `Offset: ${state.patternViewOffset}`, "", "");
        return true;
    }

    /* Jog alone: scroll track buttons by 4 positions */
    const maxScroll = NUM_TRACKS - 4;  /* Show 4 tracks at a time: positions 0, 4, 8, 12 */
    if (velocity >= 1 && velocity <= 63) {
        /* Clockwise: increment by 4 */
        state.trackScrollPosition = Math.min(state.trackScrollPosition + 4, maxScroll);
    } else if (velocity >= 65 && velocity <= 127) {
        /* Counter-clockwise: decrement by 4 */
        state.trackScrollPosition = Math.max(state.trackScrollPosition - 4, 0);
    }
    updateTrackButtonLEDs();
    return true;
}

/* ============ Step Helpers ============ */

function toggleStepNote(stepIdx, note, velocity = 100) {
    const step = getCurrentPattern(state.currentTrack).steps[stepIdx];
    const noteIdx = step.notes.indexOf(note);

    if (noteIdx >= 0) {
        step.notes.splice(noteIdx, 1);
        setParam(`track_${state.currentTrack}_step_${stepIdx}_remove_note`, String(note));
        return false;
    } else {
        if (step.notes.length < 7) {
            step.notes.push(note);
            /* Capture velocity from pad press */
            step.velocity = Math.max(1, Math.min(127, velocity));
            setParam(`track_${state.currentTrack}_step_${stepIdx}_add_note`, String(note));
            setParam(`track_${state.currentTrack}_step_${stepIdx}_velocity`, String(step.velocity));
            /* Recalculate scale detection when note is added */
            if (state.chordFollow[state.currentTrack]) {
                state.detectedScale = detectScale(state.tracks, state.chordFollow);
            }
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
    step.length = 1;
    step.paramSpark = 0;
    step.compSpark = 0;
    step.jump = -1;
    step.offset = 0;
    step.arpMode = -1;
    step.arpSpeed = -1;
    step.arpOctave = -1;
    step.arpLayer = 0;
    setParam(`track_${state.currentTrack}_step_${stepIdx}_clear`, "1");
}

/**
 * Paste copied step data to target step
 */
function pasteStep(stepIdx) {
    if (!state.stepCopyBuffer) return false;

    const step = getCurrentPattern(state.currentTrack).steps[stepIdx];
    const srcStep = state.stepCopyBuffer;

    // Copy all properties
    step.notes = [...srcStep.notes];
    step.velocity = srcStep.velocity !== undefined ? srcStep.velocity : 100;
    step.cc1 = srcStep.cc1;
    step.cc2 = srcStep.cc2;
    step.probability = srcStep.probability;
    step.condition = srcStep.condition;
    step.ratchet = srcStep.ratchet;
    step.length = srcStep.length || 1;
    step.paramSpark = srcStep.paramSpark;
    step.compSpark = srcStep.compSpark;
    step.jump = srcStep.jump;
    step.offset = srcStep.offset || 0;
    step.arpMode = srcStep.arpMode !== undefined ? srcStep.arpMode : -1;
    step.arpSpeed = srcStep.arpSpeed !== undefined ? srcStep.arpSpeed : -1;
    step.arpOctave = srcStep.arpOctave !== undefined ? srcStep.arpOctave : -1;
    step.arpLayer = srcStep.arpLayer !== undefined ? srcStep.arpLayer : 0;

    // Sync ALL parameters to DSP
    setParam(`track_${state.currentTrack}_step_${stepIdx}_clear`, "1");

    // Add notes
    for (const note of step.notes) {
        setParam(`track_${state.currentTrack}_step_${stepIdx}_add_note`, String(note));
    }

    // Sync velocity
    setParam(`track_${state.currentTrack}_step_${stepIdx}_velocity`, String(step.velocity));

    // Sync CCs
    if (step.cc1 >= 0) {
        setParam(`track_${state.currentTrack}_step_${stepIdx}_cc1`, String(step.cc1));
    }
    if (step.cc2 >= 0) {
        setParam(`track_${state.currentTrack}_step_${stepIdx}_cc2`, String(step.cc2));
    }

    // Sync probability/condition
    setParam(`track_${state.currentTrack}_step_${stepIdx}_probability`, String(step.probability));
    if (step.condition > 0) {
        const cond = CONDITIONS[step.condition];
        setParam(`track_${state.currentTrack}_step_${stepIdx}_condition_n`, String(cond.n));
        setParam(`track_${state.currentTrack}_step_${stepIdx}_condition_m`, String(cond.m));
        setParam(`track_${state.currentTrack}_step_${stepIdx}_condition_not`, cond.not ? "1" : "0");
    }

    // Sync ratchet
    if (step.ratchet > 0) {
        setParam(`track_${state.currentTrack}_step_${stepIdx}_ratchet`, String(RATCHET_VALUES[step.ratchet]));
    }

    // Sync length and offset
    setParam(`track_${state.currentTrack}_step_${stepIdx}_length`, String(step.length));
    setParam(`track_${state.currentTrack}_step_${stepIdx}_offset`, String(step.offset));

    // Sync spark conditions
    if (step.paramSpark > 0) {
        const cond = CONDITIONS[step.paramSpark];
        setParam(`track_${state.currentTrack}_step_${stepIdx}_param_spark_n`, String(cond.n));
        setParam(`track_${state.currentTrack}_step_${stepIdx}_param_spark_m`, String(cond.m));
        setParam(`track_${state.currentTrack}_step_${stepIdx}_param_spark_not`, cond.not ? "1" : "0");
    }
    if (step.compSpark > 0) {
        const cond = CONDITIONS[step.compSpark];
        setParam(`track_${state.currentTrack}_step_${stepIdx}_comp_spark_n`, String(cond.n));
        setParam(`track_${state.currentTrack}_step_${stepIdx}_comp_spark_m`, String(cond.m));
        setParam(`track_${state.currentTrack}_step_${stepIdx}_comp_spark_not`, cond.not ? "1" : "0");
    }
    if (step.jump >= 0) {
        setParam(`track_${state.currentTrack}_step_${stepIdx}_jump`, String(step.jump));
    }

    // Sync arp overrides
    setParam(`track_${state.currentTrack}_step_${stepIdx}_arp_mode`, String(step.arpMode));
    setParam(`track_${state.currentTrack}_step_${stepIdx}_arp_speed`, String(step.arpSpeed));
    setParam(`track_${state.currentTrack}_step_${stepIdx}_arp_octave`, String(step.arpOctave));
    setParam(`track_${state.currentTrack}_step_${stepIdx}_arp_layer`, String(step.arpLayer));

    displayMessage(
        undefined,
        `Pasted to Step ${stepIdx + 1}`,
        `Notes: ${notesToString(step.notes)}`,
        ""
    );

    return true;
}

/* ============ LED Updates ============ */

/**
 * Update ALL LEDs for normal mode
 */
export function updateLEDs() {
    updateStepLEDs();
    updatePadLEDs();
    updateKnobLEDs();
    updateTrackButtonLEDs();
    updateTransportLEDs();
    updateCaptureLED();
    updateBackLED();
    updateOctaveLEDs();
}

function updateStepLEDs() {
    const pattern = getCurrentPattern(state.currentTrack);
    const trackColor = TRACK_COLORS[state.currentTrack];
    const dimColor = TRACK_COLORS_DIM[state.currentTrack];

    /* When shift held, only show mode icons - no track pattern */
    if (state.shiftHeld) {
        for (let i = 0; i < NUM_STEPS; i++) {
            setLED(MoveSteps[i], Black);
        }
    }
    /* When capture held, show steps with spark conditions */
    else if (state.captureHeld) {
        for (let i = 0; i < NUM_STEPS; i++) {
            const step = pattern.steps[i];
            const hasSpark = step.paramSpark > 0 || step.compSpark > 0 || step.jump >= 0;
            const hasContent = step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0;

            if (hasSpark) {
                setLED(MoveSteps[i], Purple);
            } else if (hasContent) {
                setLED(MoveSteps[i], LightGrey);
            } else {
                setLED(MoveSteps[i], Black);
            }
        }
    }
    else {
        for (let i = 0; i < NUM_STEPS; i++) {
            let color = Black;
            const inLoop = i >= pattern.loopStart && i <= pattern.loopEnd;
            const step = pattern.steps[i];

            if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
                color = inLoop ? dimColor : Navy;
            }

            /* Length visualization when holding a step */
            if (state.heldStep >= 0 && state.heldStep !== i) {
                const heldStepData = pattern.steps[state.heldStep];
                const heldLength = heldStepData.length || 1;
                const lengthEnd = state.heldStep + heldLength - 1;
                if (i > state.heldStep && i <= lengthEnd) {
                    color = Cyan;
                }
            }

            /* Playhead */
            if (state.playing && i === state.currentPlayStep && state.heldStep < 0) {
                color = White;
            }

            /* Currently held step */
            if (i === state.heldStep) {
                color = trackColor;
            }

            /* Copied step (show VividYellow indicator) */
            if (i === state.copiedStepIdx && state.stepCopyBuffer !== null) {
                color = VividYellow;
            }

            setLED(MoveSteps[i], color);
        }
    }

    /* Step UI icons - show available modes when shift held */
    setButtonLED(MoveStep1UI, state.shiftHeld ? White : Black);         /* Set view */
    setButtonLED(MoveStep2UI, state.shiftHeld ? White : Black);   /* Channel */
    setButtonLED(MoveStep5UI, state.shiftHeld ? White : Black);          /* Speed */
    setButtonLED(MoveStep7UI, state.shiftHeld ? White : Black);   /* Swing */
    /* Transpose toggle - lit when shift held OR when current track has chordFollow */
    const trackTransposed = state.chordFollow[state.currentTrack];
    setButtonLED(MoveStep8UI, state.shiftHeld ? White : (trackTransposed ? Cyan : Black));
    /* Arp - lit when shift held OR when current track has arp enabled */
    const trackArpEnabled = state.tracks[state.currentTrack].arpMode > 0;
    setButtonLED(MoveStep11UI, state.shiftHeld ? White : (trackArpEnabled ? Cyan : Black));
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

    if (state.heldStep >= 0) {
        const step = getCurrentPattern(state.currentTrack).steps[state.heldStep];
        setButtonLED(MoveKnobLEDs[0], step.cc1 >= 0 ? trackColor : LightGrey);
        setButtonLED(MoveKnobLEDs[1], step.cc2 >= 0 ? trackColor : LightGrey);
        /* Knob 3: arp (color indicates mode/speed/octave selection) */
        setButtonLED(MoveKnobLEDs[2], STEP_ARP_COLORS[state.stepArpParam]);
        setButtonLED(MoveKnobLEDs[3], Black);
        setButtonLED(MoveKnobLEDs[4], Black);
        setButtonLED(MoveKnobLEDs[5], Black);
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

function updateTrackButtonLEDs() {
    for (let i = 0; i < 4; i++) {
        const btnPosition = 3 - i;  /* Top = 0, bottom = 3 */
        const trackIdx = getTrackAtButton(btnPosition);

        if (trackIdx < 0 || trackIdx >= NUM_TRACKS) {
            setButtonLED(MoveTracks[i], Black);
            continue;
        }

        let color = Black;

        if (trackIdx === state.currentTrack) {
            /* Selected track - always bright */
            color = TRACK_COLORS[trackIdx];
        } else {
            /* Non-selected track - dim color (shows track identity even when empty) */
            color = TRACK_COLORS_DIM[trackIdx];
        }

        if (state.tracks[trackIdx] && state.tracks[trackIdx].muted) {
            color = BrightRed;
        }

        setButtonLED(MoveTracks[i], color);
    }
}

function updateTransportLEDs() {
    /* Play button */
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);

    /* Loop button - shows custom loop indicator */
    const pattern = getCurrentPattern(state.currentTrack);
    const hasCustomLoop = pattern.loopStart > 0 || pattern.loopEnd < NUM_STEPS - 1;
    setButtonLED(MoveLoop, hasCustomLoop ? TRACK_COLORS[state.currentTrack] : Black);

    /* Record button */
    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
}

function updateCaptureLED() {
    /* Dim purple - available for spark mode */
    setButtonLED(MoveCapture, DarkPurple);
}

function updateBackLED() {
    /* Back button off in track view - we're already home */
    setButtonLED(MoveBack, Black);
}

function updateOctaveLEDs() {
    /* Up/Down buttons for octave shift - lit when can move in that direction */
    setButtonLED(MoveUp, state.padOctaveOffset < 2 ? White : DarkGrey);
    setButtonLED(MoveDown, state.padOctaveOffset > -2 ? White : DarkGrey);
}

/* ============ Playhead Update ============ */

/**
 * Lightweight playhead update - only updates the two step LEDs that changed
 */
export function updatePlayhead(oldStep, newStep) {
    const pattern = getCurrentPattern(state.currentTrack);
    const dimColor = TRACK_COLORS_DIM[state.currentTrack];

    /* Restore old step to its normal color */
    if (oldStep >= 0 && oldStep < NUM_STEPS) {
        const inLoop = oldStep >= pattern.loopStart && oldStep <= pattern.loopEnd;
        const step = pattern.steps[oldStep];
        let color = Black;
        if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
            color = inLoop ? dimColor : Navy;
        }
        setLED(MoveSteps[oldStep], color);
    }

    /* Set new step to playhead color */
    if (newStep >= 0 && newStep < NUM_STEPS && state.heldStep < 0) {
        setLED(MoveSteps[newStep], White);
    }
}

/* ============ Display ============ */

export function updateDisplayContent() {
    /* Shift shows track settings */
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
        const track = state.tracks[state.currentTrack];
        const pattern = getCurrentPattern(state.currentTrack);
        const isOff = track.currentPattern < 0;
        const loopStr = (!isOff && (pattern.loopStart > 0 || pattern.loopEnd < NUM_STEPS - 1))
            ? `Loop:${pattern.loopStart + 1}-${pattern.loopEnd + 1}`
            : "";
        const patternStr = isOff ? "OFF" : `Pattern ${track.currentPattern + 1}`;

        displayMessage(
            `Track ${trackNum}${muteStr}`,
            patternStr,
            loopStr,
            ""
        );
    }
}

/* ============ Lifecycle ============ */

export function onEnter() {
    updateDisplayContent();
}

export function onExit() {
    state.heldStep = -1;
    state.stepCopyBuffer = null;
    state.copiedStepIdx = -1;
}
