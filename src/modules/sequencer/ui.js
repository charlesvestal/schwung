/*
 * SEQOMD UI Router
 * Thin router that delegates to view modules
 */

import * as std from 'std';
import * as os from 'os';

import {
    Black,
    MidiNoteOn, MidiNoteOff, MidiCC,
    MovePlay, MoveRec, MoveShift, MoveMenu, MoveBack,
    MovePads, MoveSteps, MoveCopy
} from "../../shared/constants.mjs";

import {
    isNoiseMessage, isCapacitiveTouchMessage,
    setLED, setButtonLED, clearAllLEDs, clearLEDCache,
    setLedsEnabled, getLedsEnabled
} from "../../shared/input_filter.mjs";

/* Import lib modules */
import { NUM_STEPS, TRACK_COLORS } from './lib/constants.js';
import { state, enterSetView, enterTrackView, enterPatternView, enterMasterView } from './lib/state.js';
import { setParam, getCurrentPattern } from './lib/helpers.js';
import { createEmptyTracks } from './lib/data.js';
import { loadAllSetsFromDisk, initializeSets } from './lib/persistence.js';

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

/* ============ Lifecycle ============ */

globalThis.init = function() {
    console.log("SEQOMD starting...");
    clearAllLEDs();

    /* Initialize sets */
    initializeSets();
    loadAllSetsFromDisk();

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

/* Track which pads are lit for playback display */
globalThis.tick = function() {
    drawUI();

    /* Poll DSP for playhead position when playing */
    if (state.playing && state.heldStep < 0) {
        const stepStr = host_module_get_param(`track_${state.currentTrack}_current_step`);
        const newStep = stepStr ? parseInt(stepStr, 10) : -1;

        if (newStep !== state.currentPlayStep) {
            const oldStep = state.currentPlayStep;
            state.currentPlayStep = newStep;

            /* Lightweight playhead update - only 2 step LEDs */
            if (state.view === 'track') {
                trackView.updatePlayhead(oldStep, newStep);
            }

            /* Update pad note display - only changed pads */
            if (state.view === 'track' && state.trackMode === 'normal') {
                /* Clear previously lit pads */
                for (const padIdx of state.litPads) {
                    if (padIdx >= 0 && padIdx < 32) {
                        setLED(MovePads[padIdx], Black);
                    }
                }
                state.litPads = [];

                /* Light up pads for currently playing notes */
                if (newStep >= 0 && newStep < NUM_STEPS) {
                    const step = getCurrentPattern(state.currentTrack).steps[newStep];
                    const trackColor = TRACK_COLORS[state.currentTrack];

                    for (const note of step.notes) {
                        const padIdx = note - 36;
                        if (padIdx >= 0 && padIdx < 32) {
                            setLED(MovePads[padIdx], trackColor);
                            state.litPads.push(padIdx);
                        }
                    }
                }
            }
        }
    } else if (state.currentPlayStep !== -1 && !state.playing) {
        /* Stopped - clear playhead and note display, full refresh needed */
        const oldStep = state.currentPlayStep;
        state.currentPlayStep = -1;
        state.lastRecordedStep = -1;
        for (const padIdx of state.litPads) {
            if (padIdx >= 0 && padIdx < 32) {
                setLED(MovePads[padIdx], Black);
            }
        }
        state.litPads = [];
        if (state.view === 'track') {
            /* Restore old playhead step to normal color */
            trackView.updatePlayhead(oldStep, -1);
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
            updateAllLEDs();
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
                updateAllLEDs();
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
