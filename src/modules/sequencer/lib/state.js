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

    /* Track view sub-mode: 'normal' | 'loop' | 'spark' | 'swing' | 'bpm' | 'arp' */
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
    recHeld: false,            // Rec button currently held
    heldPads: new Set(),       // Currently held pads
    lastRecordedStep: -1,      // Last step we recorded to

    /* Pattern snapshots (16 slots, each stores pattern indices for all tracks) */
    patternSnapshots: [],      // Array of 16 slots, each null or [pattern indices]

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
    sets: [],

    /* Display lines */
    line1: "SEQOMD",
    line2: "",
    line3: "",
    line4: "",

    /* Playback pad tracking */
    litPads: [],

    /* Transpose sequence (global, max 16 steps) */
    transposeSequence: [],

    /* Chord follow per track (true = transpose applies) */
    chordFollow: [false, false, false, false, true, true, true, true, false, false, false, false, true, true, true, true],

    /* Track scroll position for 16-track navigation (0-12, shows 3 tracks at a time after selected) */
    trackScrollPosition: 0,

    /* Sequencer type (0 for now, extensible) */
    sequencerType: 0,

    /* Transpose display octave offset (-2 to +2) */
    transposeOctaveOffset: 0,

    /* Current beat position in transpose sequence */
    currentTransposeBeat: 0,

    /* Cached scale detection result */
    detectedScale: null,

    /* Master view: held transpose step for editing (-1 = none) */
    heldTransposeStep: -1
};

/* Initialize track CC values */
for (let t = 0; t < NUM_TRACKS; t++) {
    state.trackCCValues.push(64, 64);
}

/* ============ Display Helper ============ */

export function displayMessage(l1, l2, l3, l4) {
    if (l1 !== undefined) state.line1 = l1;
    if (l2 !== undefined) state.line2 = l2;
    if (l3 !== undefined) state.line3 = l3;
    if (l4 !== undefined) state.line4 = l4;
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

export function isArpMode() {
    return state.view === 'track' && state.trackMode === 'arp';
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

export function enterSpeedMode() {
    state.view = 'track';
    state.trackMode = 'speed';
}

export function exitSpeedMode() {
    state.trackMode = 'normal';
}

export function enterChannelMode() {
    state.view = 'track';
    state.trackMode = 'channel';
}

export function exitChannelMode() {
    state.trackMode = 'normal';
}

export function enterArpMode() {
    state.view = 'track';
    state.trackMode = 'arp';
}

export function exitArpMode() {
    state.trackMode = 'normal';
}
