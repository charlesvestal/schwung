/*
 * SEQOMD Helpers
 * Shared utility functions used by views and router
 */

import { VividYellow, LightGrey, DarkGrey, White, Black, BrightGreen, BrightRed, MovePads, MoveSteps, MoveTracks, MoveLoop, MoveCapture, MovePlay, MoveRec, MoveBack } from '../../../shared/constants.mjs';
import { setLED, setButtonLED } from '../../../shared/input_filter.mjs';
import { NUM_TRACKS, NUM_STEPS, NUM_PATTERNS, SPEED_OPTIONS, RATCHET_VALUES, CONDITIONS, TRACK_COLORS, MoveKnobLEDs } from './constants.js';
import { state } from './state.js';
import { isRoot, isInScale } from './scale_detection.js';

/* ============ DSP Communication ============ */

/**
 * Set a parameter on the DSP plugin
 */
export function setParam(key, value) {
    host_module_set_param(key, value);
}

/**
 * Get a parameter from the DSP plugin
 */
export function getParam(key) {
    return host_module_get_param(key);
}

/* ============ BPM Helpers ============ */

/**
 * Update BPM and sync to DSP
 * @param {number} newBpm - New BPM value (clamped to 20-300)
 */
export function updateBpm(newBpm) {
    state.bpm = Math.max(20, Math.min(300, newBpm));
    setParam("bpm", String(state.bpm));
}

/* ============ Note Helpers ============ */

/**
 * Convert MIDI note number to note name
 */
export function noteToName(n) {
    if (n <= 0) return "---";
    const names = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
    return names[n % 12] + (Math.floor(n / 12) - 1);
}

/**
 * Format array of notes as string
 */
export function notesToString(notes) {
    if (!notes || notes.length === 0) return "---";
    return notes.map(n => noteToName(n)).join(" ");
}

/* ============ CC Helpers ============ */

/**
 * Send CC to external MIDI (via DSP)
 */
export function sendCCExternal(cc, value, channel) {
    setParam(`send_cc_${channel}_${cc}`, String(value));
}

/**
 * Update CC value based on encoder movement and send it
 */
export function updateAndSendCC(ccValues, index, velocity, cc, channel) {
    let val = ccValues[index];
    if (velocity >= 1 && velocity <= 63) {
        val = Math.min(val + 1, 127);
    } else if (velocity >= 65 && velocity <= 127) {
        val = Math.max(val - 1, 0);
    }
    ccValues[index] = val;
    sendCCExternal(cc, val, channel);
    return val;
}

/* ============ Track Helpers ============ */

/**
 * Get current pattern for a track
 */
export function getCurrentPattern(trackIdx) {
    return state.tracks[trackIdx].patterns[state.tracks[trackIdx].currentPattern];
}

/**
 * Select a track
 */
export function selectTrack(trackIdx) {
    if (trackIdx >= 0 && trackIdx < state.tracks.length) {
        state.currentTrack = trackIdx;
    }
}

/**
 * Toggle track mute
 */
export function toggleTrackMute(trackIdx) {
    if (trackIdx >= 0 && trackIdx < state.tracks.length) {
        state.tracks[trackIdx].muted = !state.tracks[trackIdx].muted;
        setParam(`track_${trackIdx}_mute`, state.tracks[trackIdx].muted ? "1" : "0");
    }
}

/* ============ Pad Color Helpers ============ */

/**
 * Get the base color for a pad based on piano layout and scale detection.
 * C notes are always VividYellow. Other notes depend on scale detection.
 */
export function getPadBaseColor(padIdx) {
    const midiNote = 36 + padIdx;
    const pitchClass = midiNote % 12;
    const detected = state.detectedScale;
    const isChordFollowTrack = state.chordFollow[state.currentTrack];

    /* C notes use the track's color for visual consistency */
    if (pitchClass === 0) {
        return TRACK_COLORS[state.currentTrack];
    }

    if (isChordFollowTrack && detected) {
        /* ChordFollow track with scale detected */
        if (isRoot(pitchClass, detected)) {
            /* Root note (non-C) */
            return White;
        } else if (isInScale(pitchClass, detected)) {
            /* In scale */
            return LightGrey;
        } else {
            /* Out of scale */
            return DarkGrey;
        }
    } else {
        /* Drum track or no scale detected - show basic piano layout */
        /* White keys: C, D, E, F, G, A, B = pitch classes 0, 2, 4, 5, 7, 9, 11 */
        const isWhiteKey = [0, 2, 4, 5, 7, 9, 11].includes(pitchClass);
        return isWhiteKey ? LightGrey : DarkGrey;
    }
}

/**
 * Update pad LEDs - shows piano layout with held step notes and playing notes highlighted
 * This is shared between track.js coordinator and normal.js sub-mode
 */
export function updatePadLEDs() {
    const trackColor = TRACK_COLORS[state.currentTrack];

    /* Get step notes if holding a step */
    const stepNotes = state.heldStep >= 0
        ? getCurrentPattern(state.currentTrack).steps[state.heldStep].notes
        : [];

    for (let i = 0; i < 32; i++) {
        const midiNote = 36 + i;

        /* Check if this note is in the held step */
        const isInStep = stepNotes.includes(midiNote);

        /* Check if this note is currently being played */
        const isPlaying = state.litPads && state.litPads.includes(midiNote);

        if (isInStep || isPlaying) {
            /* Step's notes or currently playing - track color (most prominent) */
            setLED(MovePads[i], trackColor);
        } else {
            /* Show piano layout base color */
            setLED(MovePads[i], getPadBaseColor(i));
        }
    }
}

/**
 * Sync all track data from JS state to DSP
 * Call this after loading a set
 */
export function syncAllTracksToDSP() {
    /* Sync BPM */
    setParam("bpm", String(state.bpm));

    for (let t = 0; t < NUM_TRACKS; t++) {
        const track = state.tracks[t];

        /* Sync track-level params */
        setParam(`track_${t}_channel`, String(track.channel));
        setParam(`track_${t}_mute`, track.muted ? "1" : "0");
        setParam(`track_${t}_speed`, String(SPEED_OPTIONS[track.speedIndex || 3].mult));
        setParam(`track_${t}_swing`, String(track.swing || 50));
        setParam(`track_${t}_pattern`, String(track.currentPattern));
        setParam(`track_${t}_chord_follow`, state.chordFollow[t] ? "1" : "0");

        /* Sync track-level arp settings */
        setParam(`track_${t}_arp_mode`, String(track.arpMode || 0));
        setParam(`track_${t}_arp_speed`, String(track.arpSpeed || 0));
        setParam(`track_${t}_arp_octave`, String(track.arpOctave || 0));

        /* Sync current pattern's loop points */
        const pattern = track.patterns[track.currentPattern];
        setParam(`track_${t}_loop_start`, String(pattern.loopStart));
        setParam(`track_${t}_loop_end`, String(pattern.loopEnd));

        /* Sync all steps in current pattern */
        for (let s = 0; s < NUM_STEPS; s++) {
            const step = pattern.steps[s];

            /* Clear step first, then add notes */
            setParam(`track_${t}_step_${s}_clear`, "1");

            /* Add each note */
            for (const note of step.notes) {
                setParam(`track_${t}_step_${s}_add_note`, String(note));
            }

            /* Sync velocity (default to 100 if not set for backwards compatibility) */
            const velocity = step.velocity !== undefined ? step.velocity : 100;
            setParam(`track_${t}_step_${s}_velocity`, String(velocity));

            /* Sync other step params */
            if (step.cc1 >= 0) {
                setParam(`track_${t}_step_${s}_cc1`, String(step.cc1));
            }
            if (step.cc2 >= 0) {
                setParam(`track_${t}_step_${s}_cc2`, String(step.cc2));
            }
            if (step.probability !== undefined && step.probability < 100) {
                setParam(`track_${t}_step_${s}_probability`, String(step.probability));
            }
            if (step.ratchet > 0) {
                /* ratchet is an index into RATCHET_VALUES */
                const ratchetVal = RATCHET_VALUES[step.ratchet] || 1;
                setParam(`track_${t}_step_${s}_ratchet`, String(ratchetVal));
            }
            if (step.length > 1) {
                setParam(`track_${t}_step_${s}_length`, String(step.length));
            }
            if (step.offset) {
                setParam(`track_${t}_step_${s}_offset`, String(step.offset));
            }
            /* Sync conditions */
            if (step.condition > 0) {
                const cond = CONDITIONS[step.condition];
                setParam(`track_${t}_step_${s}_condition_n`, String(cond.n));
                setParam(`track_${t}_step_${s}_condition_m`, String(cond.m));
                setParam(`track_${t}_step_${s}_condition_not`, cond.not ? "1" : "0");
            }
            if (step.paramSpark > 0) {
                const cond = CONDITIONS[step.paramSpark];
                setParam(`track_${t}_step_${s}_param_spark_n`, String(cond.n));
                setParam(`track_${t}_step_${s}_param_spark_m`, String(cond.m));
                setParam(`track_${t}_step_${s}_param_spark_not`, cond.not ? "1" : "0");
            }
            if (step.compSpark > 0) {
                const cond = CONDITIONS[step.compSpark];
                setParam(`track_${t}_step_${s}_comp_spark_n`, String(cond.n));
                setParam(`track_${t}_step_${s}_comp_spark_m`, String(cond.m));
                setParam(`track_${t}_step_${s}_comp_spark_not`, cond.not ? "1" : "0");
            }
            if (step.jump >= 0) {
                setParam(`track_${t}_step_${s}_jump`, String(step.jump));
            }
            /* Sync arp overrides */
            if (step.arpMode !== undefined) {
                setParam(`track_${t}_step_${s}_arp_mode`, String(step.arpMode));
            }
            if (step.arpSpeed !== undefined) {
                setParam(`track_${t}_step_${s}_arp_speed`, String(step.arpSpeed));
            }
            if (step.arpOctave !== undefined) {
                setParam(`track_${t}_step_${s}_arp_octave`, String(step.arpOctave));
            }
            if (step.arpLayer !== undefined) {
                setParam(`track_${t}_step_${s}_arp_layer`, String(step.arpLayer));
            }
        }
    }

    /* Sync transpose sequence to DSP */
    syncTransposeSequenceToDSP();

    console.log("All tracks synced to DSP");
}

/**
 * Sync transpose sequence from JS state to DSP
 * DSP now handles transpose lookup internally
 */
export function syncTransposeSequenceToDSP() {
    /* Clear existing sequence in DSP */
    setParam("transpose_clear", "1");

    /* Build mapping from UI indices to DSP indices */
    const uiToDspIndex = new Map();
    let dspIdx = 0;
    for (let uiIdx = 0; uiIdx < state.transposeSequence.length; uiIdx++) {
        if (state.transposeSequence[uiIdx]) {
            uiToDspIndex.set(uiIdx, dspIdx);
            dspIdx++;
        }
    }

    /* Send each step with remapped jump indices */
    let stepIdx = 0;
    for (let uiIdx = 0; uiIdx < state.transposeSequence.length; uiIdx++) {
        const step = state.transposeSequence[uiIdx];
        if (step) {
            setParam(`transpose_step_${stepIdx}_transpose`, String(step.transpose));
            /* Duration is stored in beats in JS, but DSP wants steps (1 beat = 4 steps) */
            const durationInSteps = step.duration * 4;
            setParam(`transpose_step_${stepIdx}_duration`, String(durationInSteps));

            /* Remap jump target from UI index to DSP index */
            let jumpDsp = -1;
            if (step.jump !== undefined && step.jump >= 0 && uiToDspIndex.has(step.jump)) {
                jumpDsp = uiToDspIndex.get(step.jump);
            }

            /* Spark parameters - use remapped jump index */
            setParam(`transpose_step_${stepIdx}_jump`, String(jumpDsp));
            const cond = CONDITIONS[step.condition || 0];
            setParam(`transpose_step_${stepIdx}_condition_n`, String(cond.n));
            setParam(`transpose_step_${stepIdx}_condition_m`, String(cond.m));
            setParam(`transpose_step_${stepIdx}_condition_not`, cond.not ? "1" : "0");

            stepIdx++;
        }
    }
    setParam("transpose_step_count", String(stepIdx));
}

/* ============ LED Helpers ============ */

/**
 * Clear step LEDs, optionally showing playhead
 * @param {boolean} showPlayhead - If true, show playhead as White (default true)
 */
export function clearStepLEDs(showPlayhead = true) {
    for (let i = 0; i < NUM_STEPS; i++) {
        const isPlayhead = showPlayhead && state.playing && i === state.currentPlayStep;
        setLED(MoveSteps[i], isPlayhead ? White : Black);
    }
}

/**
 * Clear all knob LEDs
 */
export function clearKnobLEDs() {
    for (let i = 0; i < 8; i++) {
        setButtonLED(MoveKnobLEDs[i], Black);
    }
}

/**
 * Clear all track button LEDs
 */
export function clearTrackButtonLEDs() {
    for (let i = 0; i < 4; i++) {
        setButtonLED(MoveTracks[i], Black);
    }
}

/**
 * Update transport LEDs with standard pattern
 * @param {Object} overrides - Optional overrides for specific buttons
 *   - overrides.play: Override for play button (default: state.playing ? BrightGreen : Black)
 *   - overrides.rec: Override for rec button (default: state.recording ? BrightRed : Black)
 *   - overrides.loop: Override for loop button (default: Black)
 *   - overrides.capture: Override for capture button (default: Black)
 *   - overrides.back: Override for back button (default: White)
 */
export function updateStandardTransportLEDs(overrides = {}) {
    setButtonLED(MovePlay, overrides.play !== undefined ? overrides.play : (state.playing ? BrightGreen : Black));
    setButtonLED(MoveRec, overrides.rec !== undefined ? overrides.rec : (state.recording ? BrightRed : Black));
    setButtonLED(MoveLoop, overrides.loop !== undefined ? overrides.loop : Black);
    setButtonLED(MoveCapture, overrides.capture !== undefined ? overrides.capture : Black);
    setButtonLED(MoveBack, overrides.back !== undefined ? overrides.back : White);
}

/**
 * Update playhead position with two LED updates
 * @param {number} oldStep - Previous step index
 * @param {number} newStep - New step index
 * @param {Function} getOldStepColor - Optional function to get old step color (default: () => Black)
 */
export function updatePlayheadLED(oldStep, newStep, getOldStepColor = () => Black) {
    if (oldStep >= 0 && oldStep < NUM_STEPS) {
        setLED(MoveSteps[oldStep], getOldStepColor(oldStep));
    }
    if (newStep >= 0 && newStep < NUM_STEPS) {
        setLED(MoveSteps[newStep], White);
    }
}

