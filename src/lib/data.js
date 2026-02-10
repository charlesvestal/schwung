/*
 * SEQOMD Data Structures
 * Functions for creating and manipulating track/step data
 */

import { NUM_TRACKS, NUM_STEPS, NUM_PATTERNS, DEFAULT_SPEED_INDEX, DEFAULT_ARP_SPEED, DEFAULT_TRACK_LENGTH, RESET_INF, DEFAULT_GATE } from './constants.js';

/* ============ Step Data ============ */

/**
 * Create a new empty step
 */
export function createEmptyStep() {
    return {
        notes: [],         // Array of MIDI notes
        velocities: [],    // Per-note velocity (1-127), parallel to notes array
        gate: 0,           // Gate percentage (0 = use track gate, 1-100 = override)
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
        gate: srcStep.gate !== undefined ? srcStep.gate : 0,
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
 * Create a new empty pattern (64 steps max)
 */
export function createEmptyPattern() {
    const pattern = {
        steps: []
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
        steps: []
    };
    /* Clone existing steps */
    const srcStepCount = srcPattern.steps.length;
    for (let s = 0; s < srcStepCount; s++) {
        clone.steps.push(cloneStep(srcPattern.steps[s]));
    }
    /* Ensure we have at least NUM_STEPS (expand if source was smaller) */
    while (clone.steps.length < NUM_STEPS) {
        clone.steps.push(createEmptyStep());
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
        trackLength: DEFAULT_TRACK_LENGTH,  // 1-64 steps, default 16
        resetLength: RESET_INF,             // 0=INF (never reset), 1-256 steps
        gate: DEFAULT_GATE,                 // Gate percentage (1-100), default 95
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
        trackLength: srcTrack.trackLength !== undefined ? srcTrack.trackLength : DEFAULT_TRACK_LENGTH,
        resetLength: srcTrack.resetLength !== undefined ? srcTrack.resetLength : RESET_INF,
        gate: srcTrack.gate !== undefined ? srcTrack.gate : DEFAULT_GATE,
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
 * - Expands patterns from 16 to 64 steps if needed
 * - Adds speedIndex if missing
 * - Adds swing if missing
 * - Adds trackLength, resetLength if missing
 * - Adds arp fields if missing
 * - Ignores loopStart/loopEnd (removed feature)
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
        /* Ensure trackLength exists (default 16 for backward compat) */
        if (track.trackLength === undefined) {
            track.trackLength = DEFAULT_TRACK_LENGTH;
        }
        /* Ensure resetLength exists (default INF) */
        if (track.resetLength === undefined) {
            track.resetLength = RESET_INF;
        }
        /* Ensure gate exists (default 95%) */
        if (track.gate === undefined) {
            track.gate = DEFAULT_GATE;
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
        /* Migrate patterns and step fields */
        for (const pattern of track.patterns) {
            /* Expand patterns from 16 to 64 steps if needed */
            while (pattern.steps.length < NUM_STEPS) {
                pattern.steps.push(createEmptyStep());
            }
            /* Remove obsolete loopStart/loopEnd fields (ignored) */
            delete pattern.loopStart;
            delete pattern.loopEnd;

            for (const step of pattern.steps) {
                /* Migrate gate field */
                if (step.gate === undefined) {
                    step.gate = 0;
                }
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

/* ============ Sparse Serialization ============ */

/**
 * Check if a step has any non-default data worth saving
 */
export function stepHasData(step) {
    return step.notes.length > 0 ||
           step.gate !== 0 ||
           step.cc1 >= 0 ||
           step.cc2 >= 0 ||
           step.probability !== 100 ||
           step.condition !== 0 ||
           step.ratchet !== 0 ||
           step.length !== 1 ||
           step.paramSpark !== 0 ||
           step.compSpark !== 0 ||
           step.jump !== -1 ||
           step.offset !== 0 ||
           step.arpMode !== -1 ||
           step.arpSpeed !== -1 ||
           step.arpOctave !== -1 ||
           step.arpLayer !== 0 ||
           step.arpPlaySteps !== -1 ||
           step.arpPlayStart !== -1;
}

/**
 * Serialize a step, omitting default values
 * Returns null if step is empty
 */
export function serializeStep(step) {
    if (!stepHasData(step)) return null;

    const s = {};

    /* Always include notes/velocities together if notes exist */
    if (step.notes.length > 0) {
        s.n = step.notes;
        s.v = step.velocities;
    }

    /* Only include non-default values (using short keys to save space) */
    if (step.gate !== 0) s.gt = step.gate;
    if (step.cc1 >= 0) s.c1 = step.cc1;
    if (step.cc2 >= 0) s.c2 = step.cc2;
    if (step.probability !== 100) s.pr = step.probability;
    if (step.condition !== 0) s.co = step.condition;
    if (step.ratchet !== 0) s.ra = step.ratchet;
    if (step.length !== 1) s.le = step.length;
    if (step.paramSpark !== 0) s.ps = step.paramSpark;
    if (step.compSpark !== 0) s.cs = step.compSpark;
    if (step.jump !== -1) s.ju = step.jump;
    if (step.offset !== 0) s.of = step.offset;
    if (step.arpMode !== -1) s.am = step.arpMode;
    if (step.arpSpeed !== -1) s.as = step.arpSpeed;
    if (step.arpOctave !== -1) s.ao = step.arpOctave;
    if (step.arpLayer !== 0) s.al = step.arpLayer;
    if (step.arpPlaySteps !== -1) s.ap = step.arpPlaySteps;
    if (step.arpPlayStart !== -1) s.at = step.arpPlayStart;

    return s;
}

/**
 * Deserialize a sparse step back to full format
 */
export function deserializeStep(sparse) {
    const step = createEmptyStep();

    if (!sparse) return step;

    /* Handle both old format (full keys) and new format (short keys) */
    if (sparse.notes !== undefined || sparse.n !== undefined) {
        step.notes = sparse.notes || sparse.n || [];
        step.velocities = sparse.velocities || sparse.v || [];
        /* Ensure velocities array matches notes length */
        while (step.velocities.length < step.notes.length) {
            step.velocities.push(100);
        }
    }

    /* Short keys (new format) */
    if (sparse.gt !== undefined) step.gate = sparse.gt;
    if (sparse.c1 !== undefined) step.cc1 = sparse.c1;
    if (sparse.c2 !== undefined) step.cc2 = sparse.c2;
    if (sparse.pr !== undefined) step.probability = sparse.pr;
    if (sparse.co !== undefined) step.condition = sparse.co;
    if (sparse.ra !== undefined) step.ratchet = sparse.ra;
    if (sparse.le !== undefined) step.length = sparse.le;
    if (sparse.ps !== undefined) step.paramSpark = sparse.ps;
    if (sparse.cs !== undefined) step.compSpark = sparse.cs;
    if (sparse.ju !== undefined) step.jump = sparse.ju;
    if (sparse.of !== undefined) step.offset = sparse.of;
    if (sparse.am !== undefined) step.arpMode = sparse.am;
    if (sparse.as !== undefined) step.arpSpeed = sparse.as;
    if (sparse.ao !== undefined) step.arpOctave = sparse.ao;
    if (sparse.al !== undefined) step.arpLayer = sparse.al;
    if (sparse.ap !== undefined) step.arpPlaySteps = sparse.ap;
    if (sparse.at !== undefined) step.arpPlayStart = sparse.at;

    /* Long keys (old format - for backwards compatibility) */
    if (sparse.gate !== undefined) step.gate = sparse.gate;
    if (sparse.cc1 !== undefined) step.cc1 = sparse.cc1;
    if (sparse.cc2 !== undefined) step.cc2 = sparse.cc2;
    if (sparse.probability !== undefined) step.probability = sparse.probability;
    if (sparse.condition !== undefined) step.condition = sparse.condition;
    if (sparse.ratchet !== undefined) step.ratchet = sparse.ratchet;
    if (sparse.length !== undefined) step.length = sparse.length;
    if (sparse.paramSpark !== undefined) step.paramSpark = sparse.paramSpark;
    if (sparse.compSpark !== undefined) step.compSpark = sparse.compSpark;
    if (sparse.jump !== undefined) step.jump = sparse.jump;
    if (sparse.offset !== undefined) step.offset = sparse.offset;
    if (sparse.arpMode !== undefined) step.arpMode = sparse.arpMode;
    if (sparse.arpSpeed !== undefined) step.arpSpeed = sparse.arpSpeed;
    if (sparse.arpOctave !== undefined) step.arpOctave = sparse.arpOctave;
    if (sparse.arpLayer !== undefined) step.arpLayer = sparse.arpLayer;
    if (sparse.arpPlaySteps !== undefined) step.arpPlaySteps = sparse.arpPlaySteps;
    if (sparse.arpPlayStart !== undefined) step.arpPlayStart = sparse.arpPlayStart;

    return step;
}

/**
 * Serialize a pattern to sparse format
 * Returns null if pattern has no data
 */
export function serializePattern(pattern) {
    const steps = {};
    let hasData = false;

    for (let i = 0; i < pattern.steps.length; i++) {
        const serialized = serializeStep(pattern.steps[i]);
        if (serialized) {
            steps[i] = serialized;
            hasData = true;
        }
    }

    return hasData ? { s: steps } : null;
}

/**
 * Deserialize a sparse pattern back to full format
 */
export function deserializePattern(sparse) {
    const pattern = createEmptyPattern();

    if (!sparse) return pattern;

    /* Handle both old format (steps array) and new format (s object) */
    if (Array.isArray(sparse.steps)) {
        /* Old format: full steps array */
        for (let i = 0; i < sparse.steps.length && i < NUM_STEPS; i++) {
            pattern.steps[i] = deserializeStep(sparse.steps[i]);
        }
    } else if (sparse.s) {
        /* New format: sparse steps object */
        for (const idx in sparse.s) {
            const i = parseInt(idx);
            if (i >= 0 && i < NUM_STEPS) {
                pattern.steps[i] = deserializeStep(sparse.s[idx]);
            }
        }
    }

    return pattern;
}

/**
 * Get default channel for a track index
 */
function getDefaultChannel(trackIndex) {
    return trackIndex < 8 ? trackIndex : 9;
}

/**
 * Serialize a track to sparse format
 */
export function serializeTrack(track, trackIndex) {
    const t = {};

    /* Only include non-default track settings */
    if (track.currentPattern !== 0) t.cp = track.currentPattern;
    if (track.muted) t.mu = true;
    if (track.channel !== getDefaultChannel(trackIndex)) t.ch = track.channel;
    if (track.speedIndex !== DEFAULT_SPEED_INDEX) t.sp = track.speedIndex;
    if (track.swing !== 50) t.sw = track.swing;
    if (track.trackLength !== DEFAULT_TRACK_LENGTH) t.tl = track.trackLength;
    if (track.resetLength !== RESET_INF) t.rl = track.resetLength;
    if (track.gate !== DEFAULT_GATE) t.ga = track.gate;
    if (track.arpMode !== 0) t.am = track.arpMode;
    if (track.arpSpeed !== DEFAULT_ARP_SPEED) t.as = track.arpSpeed;
    if (track.arpOctave !== 0) t.ao = track.arpOctave;
    if (track.arpContinuous !== 0) t.ac = track.arpContinuous;
    if (track.arpPlaySteps !== 1) t.ap = track.arpPlaySteps;
    if (track.arpPlayStart !== 0) t.at = track.arpPlayStart;
    if (track.cc1Default !== 64) t.c1 = track.cc1Default;
    if (track.cc2Default !== 64) t.c2 = track.cc2Default;

    /* Serialize patterns sparsely */
    const patterns = {};
    let hasPatterns = false;
    for (let i = 0; i < track.patterns.length; i++) {
        const serialized = serializePattern(track.patterns[i]);
        if (serialized) {
            patterns[i] = serialized;
            hasPatterns = true;
        }
    }
    if (hasPatterns) {
        t.p = patterns;
    }

    return t;
}

/**
 * Deserialize a sparse track back to full format
 */
export function deserializeTrack(sparse, trackIndex) {
    const track = createEmptyTrack(getDefaultChannel(trackIndex));

    if (!sparse) return track;

    /* Check if this is old format (has 'patterns' array) or new format (has 'p' object) */
    const isOldFormat = Array.isArray(sparse.patterns);

    if (isOldFormat) {
        /* Old format: full track with patterns array */
        if (sparse.currentPattern !== undefined) track.currentPattern = sparse.currentPattern;
        if (sparse.muted !== undefined) track.muted = sparse.muted;
        if (sparse.channel !== undefined) track.channel = sparse.channel;
        if (sparse.speedIndex !== undefined) track.speedIndex = sparse.speedIndex;
        if (sparse.swing !== undefined) track.swing = sparse.swing;
        if (sparse.trackLength !== undefined) track.trackLength = sparse.trackLength;
        if (sparse.resetLength !== undefined) track.resetLength = sparse.resetLength;
        if (sparse.gate !== undefined) track.gate = sparse.gate;
        if (sparse.arpMode !== undefined) track.arpMode = sparse.arpMode;
        if (sparse.arpSpeed !== undefined) track.arpSpeed = sparse.arpSpeed;
        if (sparse.arpOctave !== undefined) track.arpOctave = sparse.arpOctave;
        if (sparse.arpContinuous !== undefined) track.arpContinuous = sparse.arpContinuous;
        if (sparse.arpPlaySteps !== undefined) track.arpPlaySteps = sparse.arpPlaySteps;
        if (sparse.arpPlayStart !== undefined) track.arpPlayStart = sparse.arpPlayStart;
        if (sparse.cc1Default !== undefined) track.cc1Default = sparse.cc1Default;
        if (sparse.cc2Default !== undefined) track.cc2Default = sparse.cc2Default;

        /* Deserialize patterns array */
        for (let i = 0; i < sparse.patterns.length && i < NUM_PATTERNS; i++) {
            track.patterns[i] = deserializePattern(sparse.patterns[i]);
        }
    } else {
        /* New format: sparse track */
        if (sparse.cp !== undefined) track.currentPattern = sparse.cp;
        if (sparse.mu !== undefined) track.muted = sparse.mu;
        if (sparse.ch !== undefined) track.channel = sparse.ch;
        if (sparse.sp !== undefined) track.speedIndex = sparse.sp;
        if (sparse.sw !== undefined) track.swing = sparse.sw;
        if (sparse.tl !== undefined) track.trackLength = sparse.tl;
        if (sparse.rl !== undefined) track.resetLength = sparse.rl;
        if (sparse.ga !== undefined) track.gate = sparse.ga;
        if (sparse.am !== undefined) track.arpMode = sparse.am;
        if (sparse.as !== undefined) track.arpSpeed = sparse.as;
        if (sparse.ao !== undefined) track.arpOctave = sparse.ao;
        if (sparse.ac !== undefined) track.arpContinuous = sparse.ac;
        if (sparse.ap !== undefined) track.arpPlaySteps = sparse.ap;
        if (sparse.at !== undefined) track.arpPlayStart = sparse.at;
        if (sparse.c1 !== undefined) track.cc1Default = sparse.c1;
        if (sparse.c2 !== undefined) track.cc2Default = sparse.c2;

        /* Deserialize sparse patterns object */
        if (sparse.p) {
            for (const idx in sparse.p) {
                const i = parseInt(idx);
                if (i >= 0 && i < NUM_PATTERNS) {
                    track.patterns[i] = deserializePattern(sparse.p[idx]);
                }
            }
        }
    }

    return track;
}

/**
 * Serialize all tracks to sparse format
 */
export function serializeTracks(tracks) {
    const result = {};
    let hasData = false;

    for (let i = 0; i < tracks.length; i++) {
        const serialized = serializeTrack(tracks[i], i);
        /* Always include track if it has any non-default settings or pattern data */
        if (Object.keys(serialized).length > 0) {
            result[i] = serialized;
            hasData = true;
        }
    }

    return hasData ? result : {};
}

/**
 * Deserialize sparse tracks back to full format
 */
export function deserializeTracks(sparse) {
    const tracks = createEmptyTracks();

    if (!sparse) return tracks;

    /* Check if this is old format (array) or new format (object) */
    if (Array.isArray(sparse)) {
        /* Old format: full tracks array */
        for (let i = 0; i < sparse.length && i < NUM_TRACKS; i++) {
            tracks[i] = deserializeTrack(sparse[i], i);
        }
    } else {
        /* New format: sparse tracks object */
        for (const idx in sparse) {
            const i = parseInt(idx);
            if (i >= 0 && i < NUM_TRACKS) {
                tracks[i] = deserializeTrack(sparse[idx], i);
            }
        }
    }

    /* Run migration to ensure all fields exist */
    return migrateTrackData(tracks);
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
