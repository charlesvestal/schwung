/*
 * Track View - Arp Mode
 * Adjust track arpeggiator settings via knobs
 *
 * This mode is fully self-contained:
 * - Owns ALL LEDs
 * - Handles knob input for mode, speed, octave
 * - No track selection, no other controls
 */

import {
    Cyan, BrightGreen, BrightRed, Black, White, VividYellow, Purple, Rose, LightGrey,
    MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack,
    MoveStep11UI, MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6
} from "../../lib/shared-constants.js";

import { setLED, setButtonLED } from "../../lib/shared-input.js";
import { NUM_STEPS, MoveKnobLEDs, ARP_MODES, ARP_SPEEDS, ARP_OCTAVES, TRACK_COLORS, getRotatedPlaySteps } from '../../lib/constants.js';

/* Knob colors for each arp parameter - matches step arp mode */
const ARP_PARAM_COLORS = [Cyan, VividYellow, Purple, BrightGreen, Rose, LightGrey];
import { state, displayMessage } from '../../lib/state.js';
import { setParam, clearStepLEDs, updateStandardTransportLEDs } from '../../lib/helpers.js';
import { markDirty } from '../../lib/persistence.js';

/* ============ Input Handling ============ */

/**
 * Handle input in arp mode
 * Knobs 1-3 control arp settings
 */
export function onInput(data) {
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];
    const track = state.tracks[state.currentTrack];

    /* Knob 1 - Arp Mode */
    if (isCC && note === MoveKnob1) {
        let mode = track.arpMode;
        if (velocity >= 1 && velocity <= 63) {
            mode = Math.min(mode + 1, ARP_MODES.length - 1);
        } else if (velocity >= 65 && velocity <= 127) {
            mode = Math.max(mode - 1, 0);
        }
        track.arpMode = mode;
        setParam(`track_${state.currentTrack}_arp_mode`, String(mode));
        markDirty();
        updateDisplayContent();
        return true;
    }

    /* Knob 2 - Arp Speed */
    if (isCC && note === MoveKnob2) {
        let speed = track.arpSpeed;
        if (velocity >= 1 && velocity <= 63) {
            speed = Math.min(speed + 1, ARP_SPEEDS.length - 1);
        } else if (velocity >= 65 && velocity <= 127) {
            speed = Math.max(speed - 1, 0);
        }
        track.arpSpeed = speed;
        setParam(`track_${state.currentTrack}_arp_speed`, String(speed));
        markDirty();
        updateDisplayContent();
        return true;
    }

    /* Knob 3 - Arp Octave */
    if (isCC && note === MoveKnob3) {
        let octave = track.arpOctave;
        if (velocity >= 1 && velocity <= 63) {
            octave = Math.min(octave + 1, ARP_OCTAVES.length - 1);
        } else if (velocity >= 65 && velocity <= 127) {
            octave = Math.max(octave - 1, 0);
        }
        track.arpOctave = octave;
        setParam(`track_${state.currentTrack}_arp_octave`, String(octave));
        markDirty();
        updateDisplayContent();
        return true;
    }

    /* Knob 4 - Arp Continuous (toggle) */
    if (isCC && note === MoveKnob4) {
        /* Any turn toggles continuous mode */
        track.arpContinuous = track.arpContinuous ? 0 : 1;
        setParam(`track_${state.currentTrack}_arp_continuous`, String(track.arpContinuous));
        markDirty();
        updateDisplayContent();
        return true;
    }

    /* Knob 5 - Play Steps (1-255) */
    if (isCC && note === MoveKnob5) {
        let playSteps = track.arpPlaySteps;
        if (velocity >= 1 && velocity <= 63) {
            playSteps = Math.min(playSteps + 1, 255);
        } else if (velocity >= 65 && velocity <= 127) {
            playSteps = Math.max(playSteps - 1, 1);
        }
        track.arpPlaySteps = playSteps;
        setParam(`track_${state.currentTrack}_arp_play_steps`, String(playSteps));
        markDirty();
        updateDisplayContent();
        return true;
    }

    /* Knob 6 - Play Start (0-7) */
    if (isCC && note === MoveKnob6) {
        let playStart = track.arpPlayStart;
        if (velocity >= 1 && velocity <= 63) {
            playStart = Math.min(playStart + 1, 7);
        } else if (velocity >= 65 && velocity <= 127) {
            playStart = Math.max(playStart - 1, 0);
        }
        track.arpPlayStart = playStart;
        setParam(`track_${state.currentTrack}_arp_play_start`, String(playStart));
        markDirty();
        updateDisplayContent();
        return true;
    }

    /* Ignore all other input - mode is focused */
    return true;
}

/* ============ LED Updates ============ */

/**
 * Update ALL LEDs for arp mode
 * This mode owns all LEDs - nothing shared
 */
export function updateLEDs() {
    const track = state.tracks[state.currentTrack];

    /* Steps - clear with playhead */
    clearStepLEDs(true);

    /* Step 11 UI lit to show we're in arp mode */
    setButtonLED(MoveStep11UI, White);

    /* Pads owned by track.js coordinator */

    /* Knobs 1-6 lit with arp param colors, 7-8 off */
    for (let i = 0; i < 8; i++) {
        if (i < 6) {
            setButtonLED(MoveKnobLEDs[i], ARP_PARAM_COLORS[i]);
        } else {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
    }

    /* Track buttons - all off (no track selection in this mode) */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }

    /* Transport - standard pattern with Back lit */
    updateStandardTransportLEDs();
}

/* ============ Display ============ */

export function updateDisplayContent() {
    const track = state.tracks[state.currentTrack];
    const modeName = ARP_MODES[track.arpMode].name;
    const speedName = ARP_SPEEDS[track.arpSpeed].name;
    const octaveName = ARP_OCTAVES[track.arpOctave].name;
    const contName = track.arpContinuous ? 'On' : 'Off';
    const playStepsPattern = getRotatedPlaySteps(track.arpPlaySteps, track.arpPlayStart);

    displayMessage(
        `Arp: ${modeName}`,
        `Spd: ${speedName} Oct: ${octaveName}`,
        `Continuous: ${contName}`,
        `Play: ${playStepsPattern}`
    );
}

/* ============ Lifecycle ============ */

export function onEnter() {
    updateDisplayContent();
}

export function onExit() {
    /* Changes saved on each input, nothing to do here */
}
