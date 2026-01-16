/*
 * Track View - Step Arp Mode
 * Edit arp overrides for a single step using knobs 1-4
 *
 * This mode is fully self-contained:
 * - Owns ALL LEDs
 * - Handles knobs 1-4 for mode, speed, octave, layer
 * - No track selection, no other controls
 * - Back button exits
 */

import {
    Black, White, Cyan, VividYellow, Purple, BrightGreen, BrightRed, Rose, LightGrey,
    MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6,
    MoveKnob1Touch, MoveKnob2Touch, MoveKnob3Touch, MoveKnob4Touch, MoveKnob5Touch, MoveKnob6Touch
} from "../../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../../shared/input_filter.mjs";
import {
    NUM_STEPS, MoveKnobLEDs, TRACK_COLORS,
    ARP_MODES, ARP_SPEEDS, ARP_OCTAVES, ARP_LAYERS, getRotatedPlaySteps
} from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import { setParam, getCurrentPattern } from '../../lib/helpers.js';
import { markDirty } from '../../lib/persistence.js';

/* Knob colors for each arp parameter */
const ARP_PARAM_COLORS = [Cyan, VividYellow, Purple, BrightGreen, Rose, LightGrey];

/* Touch tracking for tap detection */
const HOLD_THRESHOLD_MS = 300;
const knobTouchTime = {};
const knobTurned = {};

/* ============ Input Handling ============ */

/**
 * Handle input in step arp mode
 * Knobs 1-4 control arp settings for the step
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /* Handle knob touch for tap detection */
    const touchKnobs = [MoveKnob1Touch, MoveKnob2Touch, MoveKnob3Touch, MoveKnob4Touch, MoveKnob5Touch, MoveKnob6Touch];
    if (isNote && touchKnobs.includes(note)) {
        const knobIdx = touchKnobs.indexOf(note);
        if (isNoteOn && velocity > 0) {
            knobTouchTime[knobIdx] = Date.now();
            knobTurned[knobIdx] = false;
        } else {
            /* Note off - check for tap */
            if (knobTouchTime[knobIdx] && !knobTurned[knobIdx]) {
                const touchDuration = Date.now() - knobTouchTime[knobIdx];
                if (touchDuration < HOLD_THRESHOLD_MS) {
                    handleKnobTap(knobIdx);
                }
            }
            delete knobTouchTime[knobIdx];
        }
        return true;
    }

    /* Handle knob turns */
    const knobs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6];
    if (isCC && knobs.includes(note)) {
        const knobIdx = knobs.indexOf(note);
        knobTurned[knobIdx] = true;
        handleKnobTurn(knobIdx, velocity);
        return true;
    }

    /* Ignore all other input - mode is focused */
    return true;
}

/**
 * Handle knob tap - reset parameter to track default
 */
function handleKnobTap(knobIdx) {
    const step = getCurrentPattern(state.currentTrack).steps[state.stepArpEditStep];

    if (knobIdx === 0) {
        /* Reset mode to track default */
        step.arpMode = -1;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_mode`, "-1");
    } else if (knobIdx === 1) {
        /* Reset speed to track default */
        step.arpSpeed = -1;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_speed`, "-1");
    } else if (knobIdx === 2) {
        /* Reset octave to track default */
        step.arpOctave = -1;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_octave`, "-1");
    } else if (knobIdx === 3) {
        /* Reset layer to default (0) */
        step.arpLayer = 0;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_layer`, "0");
    } else if (knobIdx === 4) {
        /* Reset play steps to track default */
        step.arpPlaySteps = -1;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_play_steps`, "-1");
    } else if (knobIdx === 5) {
        /* Reset play start to track default */
        step.arpPlayStart = -1;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_play_start`, "-1");
    }

    markDirty();
    updateDisplayContent();
}

/**
 * Handle knob turn - adjust parameter
 */
function handleKnobTurn(knobIdx, velocity) {
    const step = getCurrentPattern(state.currentTrack).steps[state.stepArpEditStep];

    if (knobIdx === 0) {
        /* Mode */
        let mode = step.arpMode;
        if (velocity >= 1 && velocity <= 63) {
            mode = Math.min(mode + 1, ARP_MODES.length - 1);
        } else if (velocity >= 65 && velocity <= 127) {
            mode = Math.max(mode - 1, -1);
        }
        step.arpMode = mode;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_mode`, String(mode));
    } else if (knobIdx === 1) {
        /* Speed */
        let speed = step.arpSpeed;
        if (velocity >= 1 && velocity <= 63) {
            speed = Math.min(speed + 1, ARP_SPEEDS.length - 1);
        } else if (velocity >= 65 && velocity <= 127) {
            speed = Math.max(speed - 1, -1);
        }
        step.arpSpeed = speed;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_speed`, String(speed));
    } else if (knobIdx === 2) {
        /* Octave */
        let octave = step.arpOctave;
        if (velocity >= 1 && velocity <= 63) {
            octave = Math.min(octave + 1, ARP_OCTAVES.length - 1);
        } else if (velocity >= 65 && velocity <= 127) {
            octave = Math.max(octave - 1, -1);
        }
        step.arpOctave = octave;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_octave`, String(octave));
    } else if (knobIdx === 3) {
        /* Layer */
        let layer = step.arpLayer;
        if (velocity >= 1 && velocity <= 63) {
            layer = Math.min(layer + 1, ARP_LAYERS.length - 1);
        } else if (velocity >= 65 && velocity <= 127) {
            layer = Math.max(layer - 1, 0);
        }
        step.arpLayer = layer;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_layer`, String(layer));
    } else if (knobIdx === 4) {
        /* Play Steps */
        let playSteps = step.arpPlaySteps;
        if (velocity >= 1 && velocity <= 63) {
            playSteps = Math.min(playSteps + 1, 255);
        } else if (velocity >= 65 && velocity <= 127) {
            playSteps = Math.max(playSteps - 1, -1);
        }
        step.arpPlaySteps = playSteps;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_play_steps`, String(playSteps));
    } else if (knobIdx === 5) {
        /* Play Start */
        let playStart = step.arpPlayStart;
        if (velocity >= 1 && velocity <= 63) {
            playStart = Math.min(playStart + 1, 7);
        } else if (velocity >= 65 && velocity <= 127) {
            playStart = Math.max(playStart - 1, -1);
        }
        step.arpPlayStart = playStart;
        setParam(`track_${state.currentTrack}_step_${state.stepArpEditStep}_arp_play_start`, String(playStart));
    }

    markDirty();
    updateDisplayContent();
}

/* ============ LED Updates ============ */

/**
 * Update ALL LEDs for step arp mode
 * This mode owns all LEDs - nothing shared
 */
export function updateLEDs() {
    const trackColor = TRACK_COLORS[state.currentTrack];

    /* Steps - all black except the step being edited */
    for (let i = 0; i < NUM_STEPS; i++) {
        let color = Black;

        /* The step being edited */
        if (i === state.stepArpEditStep) {
            color = trackColor;
        }

        /* Playhead overlay */
        if (state.playing && i === state.currentPlayStep) {
            color = White;
        }

        setLED(MoveSteps[i], color);
    }

    /* Pads owned by track.js coordinator */

    /* Knobs 1-6 lit with arp param colors, 7-8 off */
    for (let i = 0; i < 8; i++) {
        if (i < 6) {
            setButtonLED(MoveKnobLEDs[i], ARP_PARAM_COLORS[i]);
        } else {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
    }

    /* Track buttons - all off */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }

    /* Transport - all off except back */
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);
    setButtonLED(MoveRec, Black);
    setButtonLED(MoveLoop, Black);
    setButtonLED(MoveCapture, Black);
    setButtonLED(MoveBack, White);
}

/* ============ Playhead ============ */

/**
 * Get the color for a step in step arp mode
 */
function getStepColor(stepIdx) {
    if (stepIdx === state.stepArpEditStep) {
        return TRACK_COLORS[state.currentTrack];
    }
    return Black;
}

/**
 * Lightweight playhead update - only updates the two step LEDs that changed
 */
export function updatePlayhead(oldStep, newStep) {
    /* Restore old step to its mode color */
    if (oldStep >= 0 && oldStep < NUM_STEPS) {
        setLED(MoveSteps[oldStep], getStepColor(oldStep));
    }
    /* Set new step to playhead */
    if (newStep >= 0 && newStep < NUM_STEPS) {
        setLED(MoveSteps[newStep], White);
    }
}

/* ============ Display ============ */

export function updateDisplayContent() {
    const step = getCurrentPattern(state.currentTrack).steps[state.stepArpEditStep];
    const track = state.tracks[state.currentTrack];

    const modeName = step.arpMode < 0 ? "TR" : ARP_MODES[step.arpMode].name;
    const speedName = step.arpSpeed < 0 ? "TR" : ARP_SPEEDS[step.arpSpeed].name;
    const octaveName = step.arpOctave < 0 ? "TR" : ARP_OCTAVES[step.arpOctave].name;
    const layerName = ARP_LAYERS[step.arpLayer].name;

    /* For play steps, show "TR" if using track default, otherwise the rotated pattern */
    let playStepsDisplay;
    if (step.arpPlaySteps < 0) {
        playStepsDisplay = "TR";
    } else {
        const playStart = step.arpPlayStart >= 0 ? step.arpPlayStart : track.arpPlayStart;
        playStepsDisplay = getRotatedPlaySteps(step.arpPlaySteps, playStart);
    }

    displayMessage(
        `Arp: ${modeName}`,
        `Spd: ${speedName} Oct: ${octaveName}`,
        `Layer: ${layerName}`,
        `Play: ${playStepsDisplay}`
    );
}

/* ============ Lifecycle ============ */

export function onEnter() {
    /* Clear touch tracking */
    for (const key in knobTouchTime) delete knobTouchTime[key];
    for (const key in knobTurned) delete knobTurned[key];
    updateDisplayContent();
}

export function onExit() {
    /* Clear touch tracking */
    for (const key in knobTouchTime) delete knobTouchTime[key];
    for (const key in knobTurned) delete knobTurned[key];
}
