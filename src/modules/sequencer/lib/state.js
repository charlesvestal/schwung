/*
 * SEQOMD State
 * All mutable state for the sequencer
 */

import { NUM_TRACKS } from './constants.js';

/* ============ Main State Object ============ */

export const state = {
    /* Transport */
    playing: false,
    bpm: 120,
    sendClock: 1,

    /* Current view: 'set' | 'track' | 'pattern' | 'master' */
    view: 'set',

    /* Track view sub-mode: 'normal' | 'loop' | 'spark' | 'swing' | 'bpm' */
    trackMode: 'normal',

    /* Track selection */
    currentTrack: 0,           // 0-7

    /* Modifier keys */
    shiftHeld: false,
    captureHeld: false,

    /* Step editing */
    heldStep: -1,              // Currently held step for editing (-1 = none)
    stepPressTimes: {},        // Per-step press timestamps for quick tap detection
    stepPadPressed: {},        // Per-step flag if pad was pressed while held

    /* Playhead */
    currentPlayStep: -1,       // Current playhead position (0-15)

    /* Note selection */
    lastPlayedNote: -1,        // Last note that was played (for pad display)
    lastSelectedNote: -1,      // Last pad pressed (for quick step entry)

    /* Recording */
    recording: false,
    heldPads: new Set(),       // Currently held pads
    lastRecordedStep: -1,      // Last step we recorded to

    /* Loop editing */
    loopEditFirst: -1,         // First step pressed while in loop edit

    /* Pattern view */
    patternViewOffset: 0,      // Which row of patterns to show (0-26)

    /* Spark mode */
    sparkSelectedSteps: new Set(),  // Steps selected for spark editing

    /* Set view */
    currentSet: 0,             // Currently loaded set (0-31)

    /* CC output values */
    patternCCValues: [64, 64, 64, 64, 64, 64, 64, 64],
    trackCCValues: [],         // Initialized below

    /* Knob touch tracking */
    knobTouchTime: {},
    knobTurned: {},

    /* Track data */
    tracks: [],

    /* Set storage */
    sets: []
};

/* Initialize track CC values */
for (let t = 0; t < NUM_TRACKS; t++) {
    state.trackCCValues.push(64, 64);
}

/* ============ Legacy Compatibility ============ */
/* These map old boolean flags to the new view/mode system */

export function isSetView() {
    return state.view === 'set';
}

export function isPatternMode() {
    return state.view === 'pattern';
}

export function isMasterMode() {
    return state.view === 'master';
}

export function isLoopEditMode() {
    return state.view === 'track' && state.trackMode === 'loop';
}

export function isSparkMode() {
    return state.view === 'track' && state.trackMode === 'spark';
}

export function isSwingMode() {
    return state.view === 'track' && state.trackMode === 'swing';
}

export function isBpmMode() {
    return state.view === 'track' && state.trackMode === 'bpm';
}

export function isTrackNormalMode() {
    return state.view === 'track' && state.trackMode === 'normal';
}

/* ============ View Transitions ============ */

export function enterSetView() {
    state.view = 'set';
    state.trackMode = 'normal';
}

export function enterTrackView() {
    state.view = 'track';
    state.trackMode = 'normal';
}

export function enterPatternView() {
    state.view = 'pattern';
    state.trackMode = 'normal';
}

export function enterMasterView() {
    state.view = 'master';
    state.trackMode = 'normal';
}

export function enterLoopEdit() {
    state.view = 'track';
    state.trackMode = 'loop';
    state.loopEditFirst = -1;
}

export function exitLoopEdit() {
    state.trackMode = 'normal';
    state.loopEditFirst = -1;
}

export function enterSparkMode() {
    state.view = 'track';
    state.trackMode = 'spark';
    state.sparkSelectedSteps.clear();
}

export function exitSparkMode() {
    state.trackMode = 'normal';
    state.sparkSelectedSteps.clear();
}

export function enterSwingMode() {
    state.view = 'track';
    state.trackMode = 'swing';
}

export function exitSwingMode() {
    state.trackMode = 'normal';
}

export function enterBpmMode() {
    state.view = 'track';
    state.trackMode = 'bpm';
}

export function exitBpmMode() {
    state.trackMode = 'normal';
}
