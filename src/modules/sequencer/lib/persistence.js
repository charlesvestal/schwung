/*
 * SEQOMD Persistence
 * Save and load sets to disk
 */

import * as std from 'std';
import * as os from 'os';

import { DATA_DIR, SETS_FILE, NUM_SETS } from './constants.js';
import { state } from './state.js';
import { createEmptyTracks, deepCloneTracks, migrateTrackData } from './data.js';

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

/* ============ Disk I/O ============ */

/**
 * Save all sets to disk
 */
export function saveAllSetsToDisk() {
    ensureDataDir();
    try {
        const f = std.open(SETS_FILE, 'w');
        if (f) {
            f.puts(JSON.stringify(state.sets));
            f.close();
            console.log('Sets saved to disk');
            return true;
        }
    } catch (e) {
        console.log('Failed to save sets: ' + e);
    }
    return false;
}

/**
 * Load all sets from disk
 */
export function loadAllSetsFromDisk() {
    try {
        const content = std.loadFile(SETS_FILE);
        if (content) {
            const loaded = JSON.parse(content);
            if (Array.isArray(loaded) && loaded.length === NUM_SETS) {
                state.sets = loaded;
                console.log('Sets loaded from disk');
                return true;
            }
        }
    } catch (e) {
        console.log('No saved sets found or failed to load: ' + e);
    }
    return false;
}

/* ============ Set Operations ============ */

/**
 * Save current tracks to current set (in memory)
 */
export function saveCurrentSet() {
    state.sets[state.currentSet] = {
        tracks: deepCloneTracks(state.tracks),
        bpm: state.bpm
    };
}

/**
 * Load a set into current tracks
 * Returns the set data for syncing to DSP
 */
export function loadSetToTracks(setIdx) {
    if (!state.sets[setIdx]) {
        state.sets[setIdx] = {
            tracks: createEmptyTracks(),
            bpm: 120
        };
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
