/*
 * SEQOMD Data Structures
 * Functions for creating and manipulating track/step data
 */

import { NUM_TRACKS, NUM_STEPS, NUM_PATTERNS, DEFAULT_SPEED_INDEX, DEFAULT_ARP_SPEED } from './constants.js';

/* ============ Step Data ============ */

/**
 * Create a new empty step
 */
export function createEmptyStep() {
    return {
        notes: [],         // Array of MIDI notes
        velocities: [],    // Per-note velocity (1-127), parallel to notes array
        cc1: -1,           // CC value for knob 1 (-1 = not set)
        cc2: -1,           // CC value for knob 2 (-1 = not set)
        probability: 100,  // 1-100%
        condition: 0,      // Index into CONDITIONS (0 = none) - TRIGGER SPARK
        ratchet: 0,        // Index into RATCHET_VALUES (0 = 1x, no ratchet)
        length: 1,         // Note length in steps (1-16)
        paramSpark: 0,     // Index into CONDITIONS - when CC locks apply
        compSpark: 0,      // Index into CONDITIONS - when ratchet/jump apply
        jump: -1,          // Jump target step (-1 = no jump, 0-15 = step)
        offset: 0,         // Micro-timing offset in ticks (-24 to +24, 48 ticks per step)
        arpMode: -1,       // Arp mode override (-1 = use track, 0+ = override)
        arpSpeed: -1,      // Arp speed override (-1 = use track, 0+ = override)
        arpOctave: -1,     // Arp octave override (-1 = use track, 0+ = override)
        arpLayer: 0,       // Arp layer mode (0=Layer, 1=Cut, 2=Legato) - step only, no track default
        arpPlaySteps: -1,  // Arp play steps pattern (-1 = use track, 1-255 = binary pattern)
        arpPlayStart: -1   // Arp play steps start position (-1 = use track, 0-7 = offset)
    };
}

/**
 * Deep clone a step
 */
export function cloneStep(srcStep) {
    /* Migrate old single velocity to per-note velocities array */
    let velocities;
    if (srcStep.velocities && srcStep.velocities.length > 0) {
        velocities = [...srcStep.velocities];
    } else {
        /* Old format: create velocities array from single velocity */
        const vel = srcStep.velocity !== undefined ? srcStep.velocity : 100;
        velocities = srcStep.notes.map(() => vel);
    }
    return {
        notes: [...srcStep.notes],
        velocities: velocities,
        cc1: srcStep.cc1,
        cc2: srcStep.cc2,
        probability: srcStep.probability,
        condition: srcStep.condition,
        ratchet: srcStep.ratchet,
        length: srcStep.length || 1,
        paramSpark: srcStep.paramSpark,
        compSpark: srcStep.compSpark,
        jump: srcStep.jump,
        offset: srcStep.offset || 0,
        arpMode: srcStep.arpMode !== undefined ? srcStep.arpMode : -1,
        arpSpeed: srcStep.arpSpeed !== undefined ? srcStep.arpSpeed : -1,
        arpOctave: srcStep.arpOctave !== undefined ? srcStep.arpOctave : -1,
        arpLayer: srcStep.arpLayer !== undefined ? srcStep.arpLayer : 0,
        arpPlaySteps: srcStep.arpPlaySteps !== undefined ? srcStep.arpPlaySteps : -1,
        arpPlayStart: srcStep.arpPlayStart !== undefined ? srcStep.arpPlayStart : -1
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
        swing: 50,
        arpMode: 0,                   // 0 = Off, 1+ = arp mode
        arpSpeed: DEFAULT_ARP_SPEED,  // Index into ARP_SPEEDS
        arpOctave: 0,                 // Index into ARP_OCTAVES
        arpContinuous: 0,             // 0 = restart arp each trigger, 1 = continue from last position
        arpPlaySteps: 1,              // Arp play steps pattern (1-255, binary pattern, 1 = all play)
        arpPlayStart: 0,              // Arp play steps start position (0-7)
        cc1Default: 64,               // Track-level CC1 default (0-127)
        cc2Default: 64                // Track-level CC2 default (0-127)
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
        swing: srcTrack.swing !== undefined ? srcTrack.swing : 50,
        arpMode: srcTrack.arpMode !== undefined ? srcTrack.arpMode : 0,
        arpSpeed: srcTrack.arpSpeed !== undefined ? srcTrack.arpSpeed : DEFAULT_ARP_SPEED,
        arpOctave: srcTrack.arpOctave !== undefined ? srcTrack.arpOctave : 0,
        arpContinuous: srcTrack.arpContinuous !== undefined ? srcTrack.arpContinuous : 0,
        arpPlaySteps: srcTrack.arpPlaySteps !== undefined ? srcTrack.arpPlaySteps : 1,
        arpPlayStart: srcTrack.arpPlayStart !== undefined ? srcTrack.arpPlayStart : 0,
        cc1Default: srcTrack.cc1Default !== undefined ? srcTrack.cc1Default : 64,
        cc2Default: srcTrack.cc2Default !== undefined ? srcTrack.cc2Default : 64
    };
    for (let p = 0; p < NUM_PATTERNS; p++) {
        clone.patterns.push(clonePattern(srcTrack.patterns[p]));
    }
    return clone;
}

/* ============ Tracks Collection ============ */

/**
 * Create empty tracks structure (16 tracks)
 */
export function createEmptyTracks() {
    const tracks = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        /* Tracks 0-7: channels 0-7, Tracks 8-15: channel 9 (MIDI channel 10 - drums) */
        const channel = t < 8 ? t : 9;
        tracks.push(createEmptyTrack(channel));
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
 * - Adds missing tracks if fewer than NUM_TRACKS
 * - Adds missing patterns if track has fewer than NUM_PATTERNS
 * - Adds speedIndex if missing
 * - Adds swing if missing
 * - Adds arp fields if missing
 */
export function migrateTrackData(trackData) {
    /* Add missing tracks (for 8-track -> 16-track migration) */
    while (trackData.length < NUM_TRACKS) {
        trackData.push(createEmptyTrack(trackData.length));
    }

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
        /* Ensure arp fields exist */
        if (track.arpMode === undefined) {
            track.arpMode = 0;
        }
        if (track.arpSpeed === undefined) {
            track.arpSpeed = DEFAULT_ARP_SPEED;
        }
        if (track.arpOctave === undefined) {
            track.arpOctave = 0;
        }
        if (track.arpContinuous === undefined) {
            track.arpContinuous = 0;
        }
        if (track.arpPlaySteps === undefined) {
            track.arpPlaySteps = 1;
        }
        if (track.arpPlayStart === undefined) {
            track.arpPlayStart = 0;
        }
        /* Ensure track CC defaults exist */
        if (track.cc1Default === undefined) {
            track.cc1Default = 64;
        }
        if (track.cc2Default === undefined) {
            track.cc2Default = 64;
        }
        /* Migrate step fields */
        for (const pattern of track.patterns) {
            for (const step of pattern.steps) {
                /* Migrate arp fields */
                if (step.arpMode === undefined) {
                    step.arpMode = -1;
                }
                if (step.arpSpeed === undefined) {
                    step.arpSpeed = -1;
                }
                if (step.arpOctave === undefined) {
                    step.arpOctave = -1;
                }
                if (step.arpLayer === undefined) {
                    step.arpLayer = 0;
                }
                if (step.arpPlaySteps === undefined) {
                    step.arpPlaySteps = -1;
                }
                if (step.arpPlayStart === undefined) {
                    step.arpPlayStart = -1;
                }
                /* Migrate velocity: old single velocity -> per-note velocities array */
                if (!step.velocities) {
                    const vel = step.velocity !== undefined ? step.velocity : 100;
                    step.velocities = step.notes.map(() => vel);
                    delete step.velocity;
                }
            }
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
        transpose: transpose,    // -24 to +24 semitones
        duration: duration,      // 1-64 beats
        jump: -1,               // Jump target step (-1 = no jump, 0-15 = step index)
        condition: 0            // Index into CONDITIONS array (0-72, 0 = always)
    };
}

/**
 * Deep clone a transpose sequence
 */
export function cloneTransposeSequence(seq) {
    if (!seq) return [];
    return seq.map(step => step ? {
        transpose: step.transpose,
        duration: step.duration,
        jump: step.jump !== undefined ? step.jump : -1,
        condition: step.condition !== undefined ? step.condition : 0
    } : null).filter(s => s !== null);
}

/**
 * Migrate old transpose sequence data to current format
 * - Adds jump field if missing (defaults to -1)
 * - Adds condition field if missing (defaults to 0)
 */
export function migrateTransposeSequence(seq) {
    if (!seq) return [];
    return seq.map(step => {
        if (!step) return null;
        if (step.jump === undefined) {
            step.jump = -1;
        }
        if (step.condition === undefined) {
            step.condition = 0;
        }
        return step;
    });
}

/**
 * Default chord follow settings (tracks 1-8 melodic, 9-16 drums)
 */
export function getDefaultChordFollow() {
    /* Tracks 1-8: transpose-follow enabled (melodic), Tracks 9-16: disabled (drums) */
    return [true, true, true, true, true, true, true, true,
            false, false, false, false, false, false, false, false];
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
 * Get current pattern for a track (returns pattern 0 if track is off)
 */
export function getCurrentPattern(tracks, trackIdx) {
    const track = tracks[trackIdx];
    const patIdx = track.currentPattern < 0 ? 0 : track.currentPattern;
    return track.patterns[patIdx];
}
