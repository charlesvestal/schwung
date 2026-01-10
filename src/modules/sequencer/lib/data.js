/*
 * SEQOMD Data Structures
 * Functions for creating and manipulating track/step data
 */

import { NUM_TRACKS, NUM_STEPS, NUM_PATTERNS, DEFAULT_SPEED_INDEX } from './constants.js';

/* ============ Step Data ============ */

/**
 * Create a new empty step
 */
export function createEmptyStep() {
    return {
        notes: [],         // Array of MIDI notes
        cc1: -1,           // CC value for knob 1 (-1 = not set)
        cc2: -1,           // CC value for knob 2 (-1 = not set)
        probability: 100,  // 1-100%
        condition: 0,      // Index into CONDITIONS (0 = none) - TRIGGER SPARK
        ratchet: 0,        // Index into RATCHET_VALUES (0 = 1x, no ratchet)
        length: 1,         // Note length in steps (1-16)
        paramSpark: 0,     // Index into CONDITIONS - when CC locks apply
        compSpark: 0,      // Index into CONDITIONS - when ratchet/jump apply
        jump: -1,          // Jump target step (-1 = no jump, 0-15 = step)
        offset: 0          // Micro-timing offset in ticks (-24 to +24, 48 ticks per step)
    };
}

/**
 * Deep clone a step
 */
export function cloneStep(srcStep) {
    return {
        notes: [...srcStep.notes],
        cc1: srcStep.cc1,
        cc2: srcStep.cc2,
        probability: srcStep.probability,
        condition: srcStep.condition,
        ratchet: srcStep.ratchet,
        length: srcStep.length || 1,
        paramSpark: srcStep.paramSpark,
        compSpark: srcStep.compSpark,
        jump: srcStep.jump,
        offset: srcStep.offset || 0
    };
}

/* ============ Pattern Data ============ */

/**
 * Create a new empty pattern
 */
export function createEmptyPattern() {
    const pattern = {
        steps: [],
        loopStart: 0,
        loopEnd: NUM_STEPS - 1
    };
    for (let s = 0; s < NUM_STEPS; s++) {
        pattern.steps.push(createEmptyStep());
    }
    return pattern;
}

/**
 * Deep clone a pattern
 */
export function clonePattern(srcPattern) {
    const clone = {
        steps: [],
        loopStart: srcPattern.loopStart,
        loopEnd: srcPattern.loopEnd
    };
    for (let s = 0; s < NUM_STEPS; s++) {
        clone.steps.push(cloneStep(srcPattern.steps[s]));
    }
    return clone;
}

/* ============ Track Data ============ */

/**
 * Create a new empty track
 */
export function createEmptyTrack(channel) {
    const track = {
        patterns: [],
        currentPattern: 0,
        muted: false,
        channel: channel,
        speedIndex: DEFAULT_SPEED_INDEX,
        swing: 50
    };
    for (let p = 0; p < NUM_PATTERNS; p++) {
        track.patterns.push(createEmptyPattern());
    }
    return track;
}

/**
 * Deep clone a track
 */
export function cloneTrack(srcTrack) {
    const clone = {
        patterns: [],
        currentPattern: srcTrack.currentPattern,
        muted: srcTrack.muted,
        channel: srcTrack.channel,
        speedIndex: srcTrack.speedIndex,
        swing: srcTrack.swing !== undefined ? srcTrack.swing : 50
    };
    for (let p = 0; p < NUM_PATTERNS; p++) {
        clone.patterns.push(clonePattern(srcTrack.patterns[p]));
    }
    return clone;
}

/* ============ Tracks Collection ============ */

/**
 * Create empty tracks structure (8 tracks)
 */
export function createEmptyTracks() {
    const tracks = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        tracks.push(createEmptyTrack(t));
    }
    return tracks;
}

/**
 * Deep clone all tracks
 */
export function deepCloneTracks(srcTracks) {
    const clone = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        clone.push(cloneTrack(srcTracks[t]));
    }
    return clone;
}

/* ============ Migration ============ */

/**
 * Migrate old track data format to current format
 * - Adds missing patterns if track has fewer than NUM_PATTERNS
 * - Adds speedIndex if missing
 * - Adds swing if missing
 */
export function migrateTrackData(trackData) {
    for (const track of trackData) {
        /* Add missing patterns */
        while (track.patterns.length < NUM_PATTERNS) {
            track.patterns.push(createEmptyPattern());
        }
        /* Ensure speedIndex exists */
        if (track.speedIndex === undefined) {
            track.speedIndex = DEFAULT_SPEED_INDEX;
        }
        /* Ensure swing exists */
        if (track.swing === undefined) {
            track.swing = 50;
        }
    }
    return trackData;
}

/* ============ Transpose Sequence ============ */

/**
 * Create a new transpose step
 */
export function createTransposeStep(transpose = 0, duration = 4) {
    return {
        transpose: transpose,
        duration: duration
    };
}

/**
 * Deep clone a transpose sequence
 */
export function cloneTransposeSequence(seq) {
    if (!seq) return [];
    return seq.map(step => step ? { transpose: step.transpose, duration: step.duration } : null)
              .filter(s => s !== null);
}

/**
 * Default chord follow settings (tracks 1-4 drums, 5-8 melodic)
 */
export function getDefaultChordFollow() {
    return [false, false, false, false, true, true, true, true];
}

/* ============ Helpers ============ */

/**
 * Check if a set has any content (notes or CC values)
 */
export function setHasContent(set) {
    if (!set) return false;
    const tracks = set.tracks || set;  /* Handle both old and new format */
    for (const track of tracks) {
        for (const pattern of track.patterns) {
            for (const step of pattern.steps) {
                if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

/**
 * Get current pattern for a track
 */
export function getCurrentPattern(tracks, trackIdx) {
    return tracks[trackIdx].patterns[tracks[trackIdx].currentPattern];
}
