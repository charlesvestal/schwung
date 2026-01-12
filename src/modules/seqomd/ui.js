/*
 * SEQOMD UI Router
 * Thin router that delegates to view modules
 */

import * as std from 'std';
import * as os from 'os';

import {
    MidiNoteOn, MidiNoteOff, MidiCC,
    MovePlay, MoveRec, MoveShift, MoveMenu, MoveBack, MoveCopy
} from "../../shared/constants.mjs";

import {
    isNoiseMessage, isCapacitiveTouchMessage,
    setButtonLED, clearLEDCache,
    setLedsEnabled, getLedsEnabled
} from "../../shared/input_filter.mjs";

/* Import lib modules */
import { NUM_STEPS } from './lib/constants.js';
import { state, enterSetView, enterTrackView, enterPatternView, enterMasterView } from './lib/state.js';
import { setParam, getCurrentPattern } from './lib/helpers.js';
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

globalThis.tick = function() {
    ledTickCounter++;
    drawUI();

    /* Check for debounced saves */
    tickDirty();

    /* Poll DSP for playhead position when playing (always at full rate for timing) */
    /* Note: Transpose is now computed internally by DSP - no polling needed */
    if (state.playing && state.heldStep < 0) {
        const stepStr = host_module_get_param(`track_${state.currentTrack}_current_step`);
        const newStep = stepStr ? parseInt(stepStr, 10) : -1;

        if (newStep !== state.currentPlayStep) {
            const oldStep = state.currentPlayStep;
            state.currentPlayStep = newStep;

            /* Mark pending LED updates */
            if (state.view === 'track') {
                pendingPlayheadUpdate = { oldStep, newStep };

                /* Update litPads with currently playing MIDI notes */
                if (newStep >= 0 && newStep < NUM_STEPS) {
                    const step = getCurrentPattern(state.currentTrack).steps[newStep];
                    state.litPads = step.notes.filter(n => n >= 36 && n < 68);
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
    }

    /* Throttled LED updates */
    if (ledTickCounter % LED_UPDATE_DIVISOR === 0) {
        if (pendingPlayheadUpdate && state.view === 'track') {
            trackView.updatePlayhead(pendingPlayheadUpdate.oldStep, pendingPlayheadUpdate.newStep);
            pendingPlayheadUpdate = null;
        }
        if (pendingPadUpdate && state.view === 'track') {
            trackView.updatePadLEDs();
            pendingPadUpdate = false;
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
            state.playing = !state.playing;
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

    /* Copy button (CC 60) - toggle LED updates for performance testing */
    if (isCC && note === MoveCopy) {
        if (velocity > 0) {
            const newState = !getLedsEnabled();
            setLedsEnabled(newState);
            console.log(`LEDs ${newState ? 'ENABLED' : 'DISABLED'}`);
            /* Show feedback on display */
            state.line1 = newState ? "LEDs ENABLED" : "LEDs DISABLED";
            state.line2 = "Press Copy to toggle";
            state.line3 = "";
            state.line4 = "";
        }
        return;
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
