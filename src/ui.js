/*
 * SEQOMD UI Router
 * Thin router that delegates to view modules
 */

import * as std from 'std';
import * as os from 'os';

import {
    MidiNoteOn, MidiNoteOff, MidiCC,
    MovePlay, MoveRec, MoveShift, MoveMenu, MoveBack, MoveCopy
} from "./lib/shared-constants.js";

import {
    isNoiseMessage, isCapacitiveTouchMessage,
    setButtonLED, clearLEDCache,
    setLedsEnabled, getLedsEnabled
} from "./lib/shared-input.js";

/* Import lib modules */
import { NUM_STEPS } from './lib/constants.js';
import { state, enterSetView, enterTrackView, enterPatternView, enterMasterView } from './lib/state.js';
import { setParam, getCurrentPattern, updatePadLEDs } from './lib/helpers.js';
import { createEmptyTracks } from './lib/data.js';
import { initializeSets, ensureSetsDir, migrateFromLegacy, flushDirty, tickDirty } from './lib/persistence.js';

/* Import views */
import * as setView from './views/set.js';
import * as trackView from './views/track.js';
import * as patternView from './views/pattern.js';
import * as masterView from './views/master.js';

/* View registry */
const views = {
    set: setView,
    track: trackView,
    pattern: patternView,
    master: masterView
};

/* LED Update Throttling
 * Tick runs at 344 Hz (44100 sample rate / 128 block size).
 * At 140 BPM, 6x speed: ~1.5 LED updates per step with divisor of 4.
 * Adjust this value to balance visual smoothness vs. SPI traffic.
 */
const LED_UPDATE_DIVISOR = 4;
let ledTickCounter = 0;

function getCurrentView() {
    return views[state.view];
}

/* ============ Display ============ */

function drawUI() {
    clear_screen();
    print(2, 2, state.line1, 1);
    print(2, 18, state.line2, 1);
    print(2, 34, state.line3, 1);
    print(2, 50, state.line4, 1);
}

/* ============ Global LED Updates ============ */

function updateMenuLED() {
    setButtonLED(MoveMenu, state.view === 'master' ? 127 : 0);
}

function updateAllLEDs() {
    clearLEDCache();  /* Clear cache so full refresh sends all LEDs */
    getCurrentView().updateLEDs();
    updateMenuLED();
}

/* Call this when switching between views */
function onViewTransition() {
    getCurrentView().updateLEDs();
    updateMenuLED();
}

/* ============ Lifecycle ============ */

globalThis.init = function() {
    console.log("SEQOMD starting...");

    /* Initialize sets - migrate from legacy format if needed */
    initializeSets();
    ensureSetsDir();
    migrateFromLegacy();

    /* Initialize track data */
    state.tracks = createEmptyTracks();

    /* Enable clock output by default */
    setParam("send_clock", "1");
    state.sendClock = 1;

    /* Enable transpose sequence automation by default */
    state.transposeSequenceEnabled = true;
    setParam("transpose_sequence_enabled", "1");

    state.currentTrack = 0;
    state.currentSet = -1;

    /* Start in set view */
    enterSetView();
    getCurrentView().onEnter();

    updateAllLEDs();
};

/* Track state for playback display */
let pendingPlayheadUpdate = null;  /* { oldStep, newStep } or null */
let pendingPadUpdate = false;
let pendingTransposeStepUpdate = false;

globalThis.tick = function() {
    ledTickCounter++;
    drawUI();

    /* Check for debounced saves */
    tickDirty();

    /* Check for display auto-return in track view */
    if (state.view === 'track') {
        trackView.tick();
    }

    /* Poll DSP for playhead position when playing (always at full rate for timing) */
    if (state.playing && state.heldStep < 0) {
        const stepStr = host_module_get_param(`track_${state.currentTrack}_current_step`);
        const newStep = stepStr ? parseInt(stepStr, 10) : -1;

        /* Poll DSP for current transpose step index */
        const transposeStepStr = host_module_get_param('current_transpose_step');
        const newTransposeStep = transposeStepStr ? parseInt(transposeStepStr, 10) : -1;

        /* Update transpose step for master view */
        if (newTransposeStep !== state.currentTransposeStep) {
            state.currentTransposeStep = newTransposeStep;
            if (state.view === 'master') {
                pendingTransposeStepUpdate = true;
            }
        }

        if (newStep !== state.currentPlayStep) {
            const oldStep = state.currentPlayStep;
            state.currentPlayStep = newStep;

            /* Mark pending LED updates */
            if (state.view === 'track') {
                pendingPlayheadUpdate = { oldStep, newStep };

                /* Update litPads with currently playing MIDI notes (including held notes) */
                if (newStep >= 0 && newStep < NUM_STEPS) {
                    const pattern = getCurrentPattern(state.currentTrack);
                    const playingNotes = [];

                    /* Find the most recent CUT step (arpLayer = 1) that cancels earlier notes */
                    let lastCutStep = -1;
                    for (let s = 0; s <= newStep; s++) {
                        const step = pattern.steps[s];
                        if (step.notes.length > 0 && step.arpLayer === 1) {
                            lastCutStep = s;
                        }
                    }

                    /* Check all steps to see if their notes are still sounding */
                    for (let s = 0; s < NUM_STEPS; s++) {
                        const step = pattern.steps[s];
                        if (step.notes.length === 0) continue;

                        const length = step.length || 1;
                        const stepEnd = s + length - 1;

                        /* Note is playing if:
                         * 1. Current step is within its duration, AND
                         * 2. This step is at or after the most recent CUT step
                         */
                        if (s <= newStep && newStep <= stepEnd && s >= lastCutStep) {
                            for (const note of step.notes) {
                                if (!playingNotes.includes(note)) {
                                    playingNotes.push(note);
                                }
                            }
                        }
                    }
                    state.litPads = playingNotes;
                } else {
                    state.litPads = [];
                }
                pendingPadUpdate = true;
            }
        }
    } else if (state.currentPlayStep !== -1 && !state.playing) {
        /* Stopped - clear playhead and note display */
        const oldStep = state.currentPlayStep;
        state.currentPlayStep = -1;
        state.lastRecordedStep = -1;
        state.litPads = [];
        if (state.view === 'track') {
            pendingPlayheadUpdate = { oldStep, newStep: -1 };
            pendingPadUpdate = true;
        }
        /* Clear transpose step */
        if (state.currentTransposeStep !== -1) {
            state.currentTransposeStep = -1;
            if (state.view === 'master') {
                pendingTransposeStepUpdate = true;
            }
        }
    }

    /* Throttled LED updates */
    if (ledTickCounter % LED_UPDATE_DIVISOR === 0) {
        if (pendingPlayheadUpdate && state.view === 'track') {
            trackView.updatePlayhead(pendingPlayheadUpdate.oldStep, pendingPlayheadUpdate.newStep);
            pendingPlayheadUpdate = null;
        }
        if (pendingPadUpdate && state.view === 'track') {
            updatePadLEDs();
            pendingPadUpdate = false;
        }
        if (pendingTransposeStepUpdate && state.view === 'master') {
            masterView.updateStepLEDs();
            pendingTransposeStepUpdate = false;
        }
    }
};

/* ============ MIDI Input ============ */

globalThis.onMidiMessageInternal = function(data) {
    if (isNoiseMessage(data)) return;

    const isNote = data[0] === MidiNoteOn || data[0] === MidiNoteOff;
    const isNoteOn = data[0] === MidiNoteOn;
    const isCC = data[0] === MidiCC;
    const note = data[1];
    const velocity = data[2];

    /* Filter capacitive touch (except in track view where it's handled) */
    if (state.view !== 'track' && isCapacitiveTouchMessage(data)) return;

    /* Shift button (CC 49) - global modifier */
    if (isCC && note === MoveShift) {
        state.shiftHeld = velocity > 0;
        getCurrentView().updateLEDs();
        getCurrentView().updateDisplayContent();
        return;
    }

    /* Play button (CC 85) - global */
    if (isCC && note === MovePlay) {
        if (velocity > 0) {
            const wasPlaying = state.playing;
            state.playing = !state.playing;

            /* Starting playback: set transpose sequence enabled BEFORE playing to avoid race condition */
            if (state.playing && !wasPlaying) {
                if (state.shiftHeld) {
                    /* Shift + Play: disable transpose sequence automation */
                    state.transposeSequenceEnabled = false;
                    setParam("transpose_sequence_enabled", "0");
                } else {
                    /* Normal Play: enable transpose sequence automation */
                    state.transposeSequenceEnabled = true;
                    setParam("transpose_sequence_enabled", "1");
                }
            }

            /* Now start/stop playback */
            setParam("playing", state.playing ? "1" : "0");

            if (!state.playing) {
                state.lastRecordedStep = -1;
                flushDirty();  /* Save any changes made during playback */
            }
            getCurrentView().updateLEDs();
        }
        return;
    }

    /* Record button (CC 86) - global */
    if (isCC && note === MoveRec) {
        if (velocity > 0) {
            state.recording = !state.recording;
            state.lastRecordedStep = -1;
            getCurrentView().updateLEDs();
        }
        return;
    }

    /* Menu button (CC 50) - view switching */
    if (isCC && note === MoveMenu) {
        if (velocity > 0) {
            const oldView = state.view;
            getCurrentView().onExit();

            if (state.shiftHeld) {
                /* Shift + Menu = master mode toggle */
                if (state.view === 'master') {
                    enterTrackView();
                } else {
                    enterMasterView();
                }
            } else {
                /* Menu alone = pattern mode toggle */
                if (state.view === 'pattern') {
                    enterTrackView();
                } else if (state.view !== 'set') {
                    enterPatternView();
                }
            }

            getCurrentView().onEnter();
            onViewTransition();
        }
        return;
    }

    /* Copy button (CC 60) - handled by views for copy/paste operations */
    if (isCC && note === MoveCopy) {
        /* Let views handle copy operations - don't consume the event */
    }

    /* Back button (CC 51) - return to track view, or let track view handle it */
    if (isCC && note === MoveBack) {
        if (velocity > 0) {
            if (state.view !== 'track') {
                getCurrentView().onExit();
                enterTrackView();
                getCurrentView().onEnter();
                onViewTransition();
                return;
            }
            /* In track view - let the view handle it (for swing mode exit etc.) */
        } else {
            return;  /* Ignore button release */
        }
    }

    /* Delegate to current view */
    const handled = getCurrentView().onInput(data);

    /* If view handled it and changed views, update */
    if (handled) {
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {};
