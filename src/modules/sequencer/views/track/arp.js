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
    Cyan, BrightGreen, BrightRed, Black, White,
    MoveSteps, MoveTracks,
    MovePlay, MoveRec, MoveLoop, MoveCapture, MoveBack,
    MoveStep11UI, MoveKnob1, MoveKnob2, MoveKnob3
} from "../../../../shared/constants.mjs";

import { setLED, setButtonLED } from "../../../../shared/input_filter.mjs";
import { NUM_STEPS, MoveKnobLEDs, ARP_MODES, ARP_SPEEDS, ARP_OCTAVES } from '../../lib/constants.js';
import { state, displayMessage } from '../../lib/state.js';
import { setParam } from '../../lib/helpers.js';
import { saveCurrentSetToDisk } from '../../lib/persistence.js';

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

    /* Steps - only step 11 lit (index 10) */
    for (let i = 0; i < NUM_STEPS; i++) {
        setLED(MoveSteps[i], i === 10 ? White : Black);
    }

    /* Step 11 UI stays lit to show we're in arp mode */
    setButtonLED(MoveStep11UI, White);

    /* Pads owned by track.js coordinator */

    /* Knobs - 1, 2, 3 lit for mode, speed, octave */
    for (let i = 0; i < 8; i++) {
        if (i < 3) {
            /* Lit color indicates if arp is active */
            setButtonLED(MoveKnobLEDs[i], track.arpMode > 0 ? Cyan : White);
        } else {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
    }

    /* Track buttons - all off (no track selection in this mode) */
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }

    /* Transport - only play/rec reflect global state */
    setButtonLED(MovePlay, state.playing ? BrightGreen : Black);
    setButtonLED(MoveRec, state.recording ? BrightRed : Black);
    setButtonLED(MoveLoop, Black);

    /* Capture off, Back lit to show exit option */
    setButtonLED(MoveCapture, Black);
    setButtonLED(MoveBack, White);
}

/* ============ Display ============ */

export function updateDisplayContent() {
    const track = state.tracks[state.currentTrack];
    const modeName = ARP_MODES[track.arpMode].name;
    const speedName = ARP_SPEEDS[track.arpSpeed].name;
    const octaveName = ARP_OCTAVES[track.arpOctave].name;

    displayMessage(
        "ARP MODE",
        `Track ${state.currentTrack + 1}`,
        `${modeName} | ${speedName} | Oct: ${octaveName}`,
        "Knobs 1-3 to adjust"
    );
}

/* ============ Lifecycle ============ */

export function onEnter() {
    updateDisplayContent();
}

export function onExit() {
    /* Save changes when exiting mode */
    saveCurrentSetToDisk();
}
