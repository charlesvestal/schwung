/*
 * SEQOMD Helpers
 * Shared utility functions used by views and router
 */

import { VividYellow, LightGrey, DarkGrey, White, Black, BrightGreen, BrightRed, MovePads, MoveSteps, MoveTracks, MoveLoop, MoveCapture, MovePlay, MoveRec, MoveBack } from './shared-constants.js';
import { setLED, setButtonLED } from './shared-input.js';
import { NUM_TRACKS, NUM_STEPS, STEPS_PER_PAGE, NUM_PATTERNS, SPEED_OPTIONS, RATCHET_VALUES, CONDITIONS, TRACK_COLORS, MoveKnobLEDs } from './constants.js';
import { state } from './state.js';
import { isRoot, isInScale } from './scale_detection.js';
import { markDirty } from './persistence.js';

/* ============ DSP Communication ============ */

/* Bulk param mode: collect setParam calls and send as one SHM request.
 * Prevents multi-second freezes when syncing thousands of params (e.g. loading sets). */
let _bulkMode = false;
let _bulkBuffer = [];

export function beginBulk() {
    _bulkMode = true;
    _bulkBuffer = [];
}

export function endBulk() {
    _bulkMode = false;
    if (_bulkBuffer.length === 0) return;

    /* Serialize as key\nvalue\nkey\nvalue\n... and send in chunks under 60KB */
    let chunk = "";
    for (let i = 0; i < _bulkBuffer.length; i++) {
        const entry = _bulkBuffer[i][0] + "\n" + _bulkBuffer[i][1] + "\n";
        if (chunk.length + entry.length > 60000 && chunk.length > 0) {
            host_module_set_param("bulk_set", chunk);
            chunk = "";
        }
        chunk += entry;
    }
    if (chunk.length > 0) {
        host_module_set_param("bulk_set", chunk);
    }
    _bulkBuffer = [];
}

/**
 * Set a parameter on the DSP plugin.
 * In bulk mode, params are collected and sent together via endBulk().
 */
export function setParam(key, value) {
    if (_bulkMode) {
        _bulkBuffer.push([key, value]);
        return;
    }
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
        const midiNote = 36 + i + (state.padOctaveOffset * 12);

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
 * Sync all track data from JS state to DSP.
 * Uses bulk mode to send all params in 1-2 SHM requests instead of 1000+.
 */
export function syncAllTracksToDSP() {
    beginBulk();

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
        setParam(`track_${t}_arp_continuous`, String(track.arpContinuous || 0));
        setParam(`track_${t}_arp_play_steps`, String(track.arpPlaySteps !== undefined ? track.arpPlaySteps : 1));
        setParam(`track_${t}_arp_play_start`, String(track.arpPlayStart !== undefined ? track.arpPlayStart : 0));

        /* Sync track-level CC defaults */
        setParam(`track_${t}_cc1_default`, String(track.cc1Default !== undefined ? track.cc1Default : 64));
        setParam(`track_${t}_cc2_default`, String(track.cc2Default !== undefined ? track.cc2Default : 64));

        /* Sync track length, reset, and gate */
        setParam(`track_${t}_length`, String(track.trackLength));
        setParam(`track_${t}_reset`, String(track.resetLength));
        setParam(`track_${t}_gate`, String(track.gate));

        const pattern = track.patterns[track.currentPattern];

        /* Sync all steps in current pattern */
        for (let s = 0; s < NUM_STEPS; s++) {
            const step = pattern.steps[s];

            /* Clear step first, then add notes */
            setParam(`track_${t}_step_${s}_clear`, "1");

            /* Add each note with its velocity */
            for (let n = 0; n < step.notes.length; n++) {
                const note = step.notes[n];
                /* Get per-note velocity (migrate from old single velocity if needed) */
                let vel = 100;
                if (step.velocities && step.velocities[n] !== undefined) {
                    vel = step.velocities[n];
                } else if (step.velocity !== undefined) {
                    vel = step.velocity;  /* Old format fallback */
                }
                setParam(`track_${t}_step_${s}_add_note`, `${note},${vel}`);
            }

            /* Sync other step params */
            if (step.gate !== undefined && step.gate !== 0) {
                setParam(`track_${t}_step_${s}_gate`, String(step.gate));
            }
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
            if (step.arpPlaySteps !== undefined) {
                setParam(`track_${t}_step_${s}_arp_play_steps`, String(step.arpPlaySteps));
            }
            if (step.arpPlayStart !== undefined) {
                setParam(`track_${t}_step_${s}_arp_play_start`, String(step.arpPlayStart));
            }
        }
    }

    /* Sync transpose sequence to DSP */
    syncTransposeSequenceToDSP();

    /* Sync master reset */
    setParam("master_reset", String(state.masterReset));

    endBulk();

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
    for (let i = 0; i < STEPS_PER_PAGE; i++) {
        /* Map display position to actual step in pattern */
        const actualStep = state.currentPage * STEPS_PER_PAGE + i;
        const isPlayhead = showPlayhead && state.playing && actualStep === state.currentPlayStep;
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
 * Only updates if steps are on current page
 * @param {number} oldStep - Previous step index (absolute, 0-63)
 * @param {number} newStep - New step index (absolute, 0-63)
 * @param {Function} getOldStepColor - Optional function to get old step color (default: () => Black)
 */
export function updatePlayheadLED(oldStep, newStep, getOldStepColor = () => Black) {
    const pageStart = state.currentPage * STEPS_PER_PAGE;
    const pageEnd = pageStart + STEPS_PER_PAGE;

    /* Update old step if on current page */
    if (oldStep >= pageStart && oldStep < pageEnd) {
        const displayIdx = oldStep - pageStart;
        setLED(MoveSteps[displayIdx], getOldStepColor(oldStep));
    }
    /* Update new step if on current page */
    if (newStep >= pageStart && newStep < pageEnd) {
        const displayIdx = newStep - pageStart;
        setLED(MoveSteps[displayIdx], White);
    }
}

/* ============ Jog Wheel Helpers ============ */

/**
 * Handle jog wheel parameter adjustment with bounds
 * @param {number} velocity - MIDI velocity from jog wheel
 * @param {number} currentValue - Current parameter value
 * @param {Object} options - Configuration options
 * @param {number} options.min - Minimum value (default 0)
 * @param {number} options.max - Maximum value (required)
 * @param {number} options.step - Step size for increment/decrement (default 1)
 * @param {boolean} options.wrap - Use modulo wrapping instead of clamping (default false)
 * @returns {number} - New parameter value after adjustment
 */
export function handleJogWheelAdjustment(velocity, currentValue, options) {
    const { min = 0, max, step = 1, wrap = false } = options;

    let newValue = currentValue;

    /* Clockwise: increment */
    if (velocity >= 1 && velocity <= 63) {
        newValue = currentValue + step;
    }
    /* Counter-clockwise: decrement */
    else if (velocity >= 65 && velocity <= 127) {
        newValue = currentValue - step;
    }

    /* Apply bounds */
    if (wrap) {
        /* Modulo wrapping for circular parameters (e.g., MIDI channel) */
        const range = max - min + 1;
        newValue = ((newValue - min + range) % range) + min;
    } else {
        /* Clamping for linear parameters */
        newValue = Math.max(min, Math.min(max, newValue));
    }

    return newValue;
}

/**
 * Handle jog wheel input for track parameter adjustment
 * Generic handler for channel/speed/swing modes
 * @param {number} velocity - MIDI velocity from jog wheel
 * @param {string} stateKey - Key in track object (e.g., 'channel', 'speedIndex', 'swing')
 * @param {string} dspParam - DSP parameter name (e.g., 'channel', 'speed', 'swing')
 * @param {Object} options - Configuration options (min, max, step, wrap)
 * @param {Function} valueToDSP - Optional transform function for DSP value (e.g., speedIdx => SPEED_OPTIONS[speedIdx].mult)
 * @param {Function} updateDisplay - Function to call after update (e.g., updateDisplayContent)
 */
export function handleJogWheelTrackParam(velocity, stateKey, dspParam, options, valueToDSP = null, updateDisplay = null) {
    const track = state.tracks[state.currentTrack];
    const currentValue = track[stateKey];

    const newValue = handleJogWheelAdjustment(velocity, currentValue, options);

    /* Update state */
    track[stateKey] = newValue;

    /* Update DSP */
    const dspValue = valueToDSP ? valueToDSP(newValue) : newValue;
    setParam(`track_${state.currentTrack}_${dspParam}`, String(dspValue));

    /* Mark dirty and update display */
    markDirty();
    if (updateDisplay) {
        updateDisplay();
    }
}

