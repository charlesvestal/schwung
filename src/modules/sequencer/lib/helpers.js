/*
 * SEQOMD Helpers
 * Shared utility functions used by views and router
 */

import { NUM_TRACKS, NUM_STEPS, NUM_PATTERNS, SPEED_OPTIONS, RATCHET_VALUES, CONDITIONS } from './constants.js';
import { state } from './state.js';

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
        }
    }

    console.log("All tracks synced to DSP");
}

