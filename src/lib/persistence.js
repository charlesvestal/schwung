/*
 * SEQOMD Persistence
 * Save and load sets to disk
 */

import * as std from 'std';
import * as os from 'os';

import { DATA_DIR, SETS_DIR, SETS_FILE, NUM_SETS } from './constants.js';
import { state } from './state.js';
import { createEmptyTracks, deepCloneTracks, migrateTrackData, cloneTransposeSequence, migrateTransposeSequence, getDefaultChordFollow, serializeTracks, deserializeTracks } from './data.js';
import { syncTransposeSequenceToDSP } from './helpers.js';

/* ============ Directory Setup ============ */

/**
 * Ensure data directory exists
 */
function ensureDataDir() {
    try {
        os.mkdir('/data/UserData/move-anything-data', 0o755);
    } catch (e) {
        /* Directory may already exist */
    }
    try {
        os.mkdir(DATA_DIR, 0o755);
    } catch (e) {
        /* Directory may already exist */
    }
}

/**
 * Ensure sets directory exists
 */
export function ensureSetsDir() {
    ensureDataDir();
    try {
        os.mkdir(SETS_DIR, 0o755);
    } catch (e) {
        /* Directory may already exist */
    }
}

/**
 * Get path to set file
 */
function getSetFilePath(setIdx) {
    return SETS_DIR + '/' + setIdx + '.json';
}

/**
 * Check if a set file exists
 */
export function setFileExists(setIdx) {
    try {
        const stat = os.stat(getSetFilePath(setIdx));
        return stat[1] === 0;  // stat[1] is error code, 0 = success
    } catch (e) {
        return false;
    }
}

/**
 * List all populated sets (sets that have files on disk)
 */
export function listPopulatedSets() {
    const populated = [];
    for (let i = 0; i < NUM_SETS; i++) {
        if (setFileExists(i)) {
            populated.push(i);
        }
    }
    return populated;
}

/* ============ Individual Set I/O ============ */

/**
 * Save a single set to disk
 */
export function saveSetToDisk(setIdx, setData) {
    ensureSetsDir();
    const filePath = getSetFilePath(setIdx);
    const tmpPath = filePath + '.tmp';

    try {
        const f = std.open(tmpPath, 'w');
        if (f) {
            f.puts(JSON.stringify(setData || state.sets[setIdx]));
            f.close();
            os.rename(tmpPath, filePath);
            return true;
        }
    } catch (e) {
        console.log('Failed to save set ' + setIdx + ': ' + e);
        try { os.remove(tmpPath); } catch (e2) {}
    }
    return false;
}

/**
 * Load a single set from disk
 * Returns set data or null if not found/error
 */
export function loadSetFromDisk(setIdx) {
    try {
        const content = std.loadFile(getSetFilePath(setIdx));
        if (content) {
            return JSON.parse(content);
        }
    } catch (e) {
        console.log('Failed to load set ' + setIdx + ': ' + e);
    }
    return null;
}

/**
 * Delete a set file
 */
export function deleteSetFile(setIdx) {
    try {
        os.remove(getSetFilePath(setIdx));
        return true;
    } catch (e) {
        return false;
    }
}

/* Debounce delay in ms - save after this much idle time */
const SAVE_DEBOUNCE_MS = 500;
let lastDirtyTime = 0;

/**
 * Mark current set as dirty (needs save).
 * Save is debounced - will happen after SAVE_DEBOUNCE_MS of no changes.
 * Call tickDirty() from main loop to process saves.
 */
export function markDirty() {
    state.dirty = true;
    lastDirtyTime = Date.now();
}

/**
 * Check and save if dirty and debounce time has passed.
 * Call this from the main tick loop.
 */
export function tickDirty() {
    if (state.dirty && !state.playing) {
        const elapsed = Date.now() - lastDirtyTime;
        if (elapsed >= SAVE_DEBOUNCE_MS) {
            saveCurrentSetToDisk();
            state.dirty = false;
        }
    }
}

/**
 * Flush dirty state to disk immediately.
 * Call this when playback stops or leaving a set.
 */
export function flushDirty() {
    if (state.dirty) {
        saveCurrentSetToDisk();
        state.dirty = false;
    }
}

/**
 * Serialize transpose sequence sparsely (only non-default values)
 */
function serializeTransposeSequence(seq) {
    if (!seq || seq.length === 0) return [];
    return seq.map(step => {
        if (!step) return null;
        const s = { t: step.transpose, d: step.duration };
        if (step.jump !== -1) s.j = step.jump;
        if (step.condition !== 0) s.c = step.condition;
        return s;
    }).filter(s => s !== null);
}

/**
 * Serialize chord follow sparsely (only if differs from default)
 */
function serializeChordFollow(cf) {
    /* Default is [true x8, false x8] */
    const defaultCf = getDefaultChordFollow();
    let isDifferent = false;
    for (let i = 0; i < 16; i++) {
        if (cf[i] !== defaultCf[i]) {
            isDifferent = true;
            break;
        }
    }
    return isDifferent ? cf : null;
}

/**
 * Serialize pattern snapshots sparsely
 */
function serializePatternSnapshots(snapshots) {
    if (!snapshots || snapshots.length === 0) return null;
    const result = {};
    let hasData = false;
    for (let i = 0; i < snapshots.length; i++) {
        if (snapshots[i]) {
            result[i] = snapshots[i];
            hasData = true;
        }
    }
    return hasData ? result : null;
}

/**
 * Save current set directly to disk (no clone)
 * Fast enough for frequent saves (e.g., every jog wheel turn)
 * Uses sparse format to minimize file size
 */
export function saveCurrentSetToDisk() {
    if (state.currentSet < 0) return false;

    /* Preserve existing color if set has one */
    let existingColor = undefined;
    if (state.sets[state.currentSet] && state.sets[state.currentSet].color !== undefined) {
        existingColor = state.sets[state.currentSet].color;
    }

    /* Serialize to sparse format */
    const setData = {
        v: 2,  /* Version marker for sparse format */
        t: serializeTracks(state.tracks)
    };

    /* Only include non-default values */
    if (state.bpm !== 120) setData.bpm = state.bpm;

    const sparseTranspose = serializeTransposeSequence(state.transposeSequence);
    if (sparseTranspose.length > 0) setData.ts = sparseTranspose;

    const sparseCf = serializeChordFollow(state.chordFollow);
    if (sparseCf) setData.cf = sparseCf;

    if (state.sequencerType !== 0) setData.st = state.sequencerType;

    const sparseSnapshots = serializePatternSnapshots(state.patternSnapshots);
    if (sparseSnapshots) setData.ps = sparseSnapshots;

    if (state.activePatternSnapshot !== -1) setData.ap = state.activePatternSnapshot;
    if (state.masterReset !== 0) setData.mr = state.masterReset;

    /* Include color if it exists */
    if (existingColor !== undefined) {
        setData.color = existingColor;
    }

    return saveSetToDisk(state.currentSet, setData);
}

/* ============ Set Operations ============ */

/**
 * Save current tracks to current set (in memory)
 * Uses sparse format for consistency with disk saves
 */
export function saveCurrentSet() {
    /* Preserve existing color if set has one */
    let existingColor = undefined;
    if (state.sets[state.currentSet] && state.sets[state.currentSet].color !== undefined) {
        existingColor = state.sets[state.currentSet].color;
    }

    /* Use sparse format */
    const setData = {
        v: 2,
        t: serializeTracks(state.tracks)
    };

    if (state.bpm !== 120) setData.bpm = state.bpm;

    const sparseTranspose = serializeTransposeSequence(state.transposeSequence);
    if (sparseTranspose.length > 0) setData.ts = sparseTranspose;

    const sparseCf = serializeChordFollow(state.chordFollow);
    if (sparseCf) setData.cf = sparseCf;

    if (state.sequencerType !== 0) setData.st = state.sequencerType;

    const sparseSnapshots = serializePatternSnapshots(state.patternSnapshots);
    if (sparseSnapshots) setData.ps = sparseSnapshots;

    if (state.activePatternSnapshot !== -1) setData.ap = state.activePatternSnapshot;
    if (state.masterReset !== 0) setData.mr = state.masterReset;

    /* Restore color if it existed */
    if (existingColor !== undefined) {
        setData.color = existingColor;
    }

    state.sets[state.currentSet] = setData;
}

/**
 * Clone pattern snapshots array
 */
function clonePatternSnapshots(snapshots) {
    if (!snapshots || snapshots.length === 0) return [];
    return snapshots.map(s => s ? [...s] : null);
}

/**
 * Deserialize transpose sequence from sparse format
 */
function deserializeTransposeSequence(sparse) {
    if (!sparse || sparse.length === 0) return [];
    return sparse.map(s => {
        if (!s) return null;
        /* Handle both old format (full keys) and new format (short keys) */
        return {
            transpose: s.transpose !== undefined ? s.transpose : (s.t !== undefined ? s.t : 0),
            duration: s.duration !== undefined ? s.duration : (s.d !== undefined ? s.d : 4),
            jump: s.jump !== undefined ? s.jump : (s.j !== undefined ? s.j : -1),
            condition: s.condition !== undefined ? s.condition : (s.c !== undefined ? s.c : 0)
        };
    }).filter(s => s !== null);
}

/**
 * Deserialize pattern snapshots from sparse format
 */
function deserializePatternSnapshots(sparse) {
    if (!sparse) return [];
    /* Handle both old format (array) and new format (object) */
    if (Array.isArray(sparse)) {
        return sparse.map(s => s ? [...s] : null);
    }
    /* Sparse object format - convert to array */
    const result = [];
    for (const idx in sparse) {
        const i = parseInt(idx);
        while (result.length <= i) result.push(null);
        result[i] = sparse[idx] ? [...sparse[idx]] : null;
    }
    return result;
}

/**
 * Load a set into current tracks
 * Returns the set data for syncing to DSP
 * Handles both old format and new sparse format (v: 2)
 */
export function loadSetToTracks(setIdx) {
    /* Always load from disk to avoid stale cache */
    const diskData = loadSetFromDisk(setIdx);
    if (diskData) {
        state.sets[setIdx] = diskData;
    } else if (!state.sets[setIdx]) {
        /* Create empty set only if no disk data AND not in memory */
        state.sets[setIdx] = {
            v: 2,
            t: {},
            bpm: 120
        };
    }

    const setData = state.sets[setIdx];
    const isV2 = setData.v === 2;

    /* Deserialize tracks based on format */
    let setTracks;
    let setBpm;

    if (isV2) {
        /* New sparse format (v2) */
        setTracks = deserializeTracks(setData.t);
        setBpm = setData.bpm || 120;
    } else {
        /* Old format - handle both array and {tracks, bpm} */
        setTracks = setData.tracks || setData;
        setBpm = setData.bpm || 120;
        /* Migrate old data if needed */
        setTracks = migrateTrackData(setTracks);
    }

    state.tracks = deepCloneTracks(setTracks);
    state.bpm = setBpm;
    state.currentSet = setIdx;

    /* Load transpose sequence */
    let transposeSeq;
    if (isV2) {
        transposeSeq = deserializeTransposeSequence(setData.ts);
    } else {
        transposeSeq = setData.transposeSequence || [];
        transposeSeq = migrateTransposeSequence(transposeSeq);
    }
    state.transposeSequence = cloneTransposeSequence(transposeSeq);

    /* Sync transpose sequence to DSP (including jump/condition parameters) */
    syncTransposeSequenceToDSP();

    /* Load chord follow */
    let chordFollow;
    if (isV2) {
        chordFollow = setData.cf || getDefaultChordFollow();
    } else if (setData.chordFollow && setData.chordFollow.length >= 8) {
        chordFollow = [...setData.chordFollow];
        /* Expand 8-element array to 16 by repeating the pattern */
        while (chordFollow.length < 16) {
            chordFollow.push(chordFollow[chordFollow.length - 8]);
        }
    } else {
        chordFollow = getDefaultChordFollow();
    }
    state.chordFollow = chordFollow;

    /* Load sequencer type */
    state.sequencerType = isV2 ? (setData.st || 0) : (setData.sequencerType || 0);

    /* Load pattern snapshots */
    const snapshotData = isV2 ? setData.ps : setData.patternSnapshots;
    state.patternSnapshots = deserializePatternSnapshots(snapshotData);

    /* Load active pattern snapshot (default to -1 if not saved) */
    const activeSnapshot = isV2 ? setData.ap : setData.activePatternSnapshot;
    state.activePatternSnapshot = activeSnapshot !== undefined ? activeSnapshot : -1;

    /* Load master reset (default to 0 = INF) */
    const masterReset = isV2 ? setData.mr : setData.masterReset;
    state.masterReset = masterReset !== undefined ? masterReset : 0;

    /* Reset transpose playback position */
    state.currentTransposeBeat = 0;
    state.transposeOctaveOffset = 0;
    state.detectedScale = null;

    /* Reset page to 0 when loading a set */
    state.currentPage = 0;

    return { tracks: state.tracks, bpm: state.bpm };
}

/**
 * Initialize sets array if empty
 */
export function initializeSets() {
    if (state.sets.length === 0) {
        for (let i = 0; i < NUM_SETS; i++) {
            state.sets.push(null);
        }
    }
}

/**
 * Check if a set has any content
 * Uses file existence check for speed (if file exists, set has content)
 */
export function setHasContent(setIdx) {
    /* Fast path: check if file exists on disk */
    if (setFileExists(setIdx)) return true;

    /* Fallback: check in-memory data (for unsaved sets) */
    if (!state.sets[setIdx]) return false;
    return setDataHasContent(state.sets[setIdx]);
}

/**
 * Check if set data object has any content (notes or CC values)
 * Handles both old format and new sparse format (v2)
 */
function setDataHasContent(setData) {
    if (!setData) return false;

    /* New sparse format (v2) - if tracks object has any keys, there's content */
    if (setData.v === 2) {
        return setData.t && Object.keys(setData.t).length > 0;
    }

    /* Old format */
    const tracks = setData.tracks || setData;
    if (!Array.isArray(tracks)) return false;
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

/* ============ Migration ============ */

/**
 * Migrate from legacy sets.json to individual set files
 * Call this once at startup - it checks if migration is needed
 */
export function migrateFromLegacy() {
    try {
        /* Check if old sets.json exists */
        const content = std.loadFile(SETS_FILE);
        if (!content) return false;

        /* Check if already migrated (sets/ dir has files) */
        ensureSetsDir();
        if (listPopulatedSets().length > 0) {
            console.log('Migration skipped - sets/ already has files');
            return false;
        }

        /* Parse old format */
        const allSets = JSON.parse(content);
        if (!Array.isArray(allSets)) return false;

        /* Write each non-empty set to individual file */
        let migrated = 0;
        for (let i = 0; i < allSets.length; i++) {
            if (allSets[i] && setDataHasContent(allSets[i])) {
                saveSetToDisk(i, allSets[i]);
                migrated++;
            }
        }

        /* Rename old file to backup */
        os.rename(SETS_FILE, SETS_FILE + '.backup');
        console.log('Migrated ' + migrated + ' sets to individual files');
        return true;
    } catch (e) {
        console.log('Migration failed: ' + e);
        return false;
    }
}
