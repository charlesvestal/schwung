/*
 * SEQOMD Persistence
 * Save and load sets to disk
 */

import * as std from 'std';
import * as os from 'os';

import { DATA_DIR, SETS_DIR, SETS_FILE, NUM_SETS } from './constants.js';
import { state } from './state.js';
import { createEmptyTracks, deepCloneTracks, migrateTrackData, cloneTransposeSequence, migrateTransposeSequence, getDefaultChordFollow } from './data.js';
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
 * Save current set directly to disk (no clone)
 * Fast enough for frequent saves (e.g., every jog wheel turn)
 */
export function saveCurrentSetToDisk() {
    if (state.currentSet < 0) return false;
    /* Serialize directly from state - no deep clone needed */
    const setData = {
        tracks: state.tracks,
        bpm: state.bpm,
        transposeSequence: state.transposeSequence,
        chordFollow: state.chordFollow,
        sequencerType: state.sequencerType,
        patternSnapshots: state.patternSnapshots,
        activePatternSnapshot: state.activePatternSnapshot
    };
    return saveSetToDisk(state.currentSet, setData);
}

/* ============ Set Operations ============ */

/**
 * Save current tracks to current set (in memory)
 */
export function saveCurrentSet() {
    state.sets[state.currentSet] = {
        tracks: deepCloneTracks(state.tracks),
        bpm: state.bpm,
        transposeSequence: cloneTransposeSequence(state.transposeSequence),
        chordFollow: [...state.chordFollow],
        sequencerType: state.sequencerType,
        patternSnapshots: clonePatternSnapshots(state.patternSnapshots),
        activePatternSnapshot: state.activePatternSnapshot
    };
}

/**
 * Clone pattern snapshots array
 */
function clonePatternSnapshots(snapshots) {
    if (!snapshots || snapshots.length === 0) return [];
    return snapshots.map(s => s ? [...s] : null);
}

/**
 * Load a set into current tracks
 * Returns the set data for syncing to DSP
 */
export function loadSetToTracks(setIdx) {
    /* Try to load from disk first if not in memory */
    if (!state.sets[setIdx]) {
        const diskData = loadSetFromDisk(setIdx);
        if (diskData) {
            state.sets[setIdx] = diskData;
        } else {
            /* Create empty set */
            state.sets[setIdx] = {
                tracks: createEmptyTracks(),
                bpm: 120,
                transposeSequence: [],
                chordFollow: getDefaultChordFollow(),
                sequencerType: 0
            };
        }
    }

    /* Handle both old format (array) and new format ({tracks, bpm}) */
    const setData = state.sets[setIdx];
    let setTracks = setData.tracks || setData;
    const setBpm = setData.bpm || 120;

    /* Migrate old data if needed */
    setTracks = migrateTrackData(setTracks);

    state.tracks = deepCloneTracks(setTracks);
    state.bpm = setBpm;
    state.currentSet = setIdx;

    /* Load transpose/chord follow with defaults for old sets */
    let transposeSeq = setData.transposeSequence || [];
    transposeSeq = migrateTransposeSequence(transposeSeq);  /* Add jump/condition fields if missing */
    state.transposeSequence = cloneTransposeSequence(transposeSeq);

    /* Sync transpose sequence to DSP (including jump/condition parameters) */
    syncTransposeSequenceToDSP();

    /* Handle chordFollow migration (8 -> 16 tracks) */
    if (setData.chordFollow && setData.chordFollow.length >= 8) {
        const chordFollow = [...setData.chordFollow];
        /* Expand 8-element array to 16 by repeating the pattern */
        while (chordFollow.length < 16) {
            chordFollow.push(chordFollow[chordFollow.length - 8]);
        }
        state.chordFollow = chordFollow;
    } else {
        state.chordFollow = getDefaultChordFollow();
    }
    state.sequencerType = setData.sequencerType || 0;

    /* Load pattern snapshots with default empty array */
    state.patternSnapshots = setData.patternSnapshots ?
        setData.patternSnapshots.map(s => s ? [...s] : null) : [];

    /* Load active pattern snapshot (default to -1 if not saved) */
    state.activePatternSnapshot = setData.activePatternSnapshot !== undefined ?
        setData.activePatternSnapshot : -1;

    /* Reset transpose playback position */
    state.currentTransposeBeat = 0;
    state.transposeOctaveOffset = 0;
    state.detectedScale = null;

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
 */
function setDataHasContent(setData) {
    if (!setData) return false;
    const tracks = setData.tracks || setData;
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
