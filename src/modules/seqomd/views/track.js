/*
 * Track View - Coordinator
 * Routes input and updates to mode-specific handlers
 * Sub-modes: normal, loop, spark, channel, speed, swing, arp
 *
 * LED Ownership:
 * - Coordinator owns: PADS (piano layout, playing notes, held step notes)
 * - Sub-modes own: steps, knobs, track buttons, transport, capture, back
 *
 * This prevents flicker on mode transitions since pads never change.
 */

import {
    MoveLoop, MoveCapture, MoveMainButton, MoveBack
} from "../../../shared/constants.mjs";

import { updatePadLEDs, setParam } from '../lib/helpers.js';

import {
    state, displayMessage, enterSetView,
    enterLoopEdit, exitLoopEdit,
    enterSparkMode, exitSparkMode,
    enterSwingMode, exitSwingMode,
    enterSpeedMode, exitSpeedMode,
    enterChannelMode, exitChannelMode,
    enterArpMode, exitArpMode
} from '../lib/state.js';

import { markDirty } from '../lib/persistence.js';
import * as setView from './set.js';

/* Mode modules */
import * as normal from './track/normal.js';
import * as loop from './track/loop.js';
import * as spark from './track/spark.js';
import * as swing from './track/swing.js';
import * as speed from './track/speed.js';
import * as channel from './track/channel.js';
import * as arp from './track/arp.js';

const modes = { normal, loop, spark, swing, speed, channel, arp };

/* ============ View Interface ============ */

/**
 * Called when entering track view
 */
export function onEnter() {
    modes[state.trackMode].onEnter();
    updateLEDs();
}

/**
 * Called when exiting track view
 */
export function onExit() {
    modes[state.trackMode].onExit();
}

/**
 * Handle MIDI input for track view
 * Returns true if handled, false to let router handle
 */
export function onInput(data) {
    const isNote = data[0] === 0x90 || data[0] === 0x80;
    const isNoteOn = data[0] === 0x90;
    const isCC = data[0] === 0xB0;
    const note = data[1];
    const velocity = data[2];

    /*
     * Mode ENTRY transitions (from normal mode only)
     * These are handled by coordinator because they change modes
     */

    /* Shift + Step 1 goes to set view */
    if (state.trackMode === 'normal' && state.shiftHeld && isNote && note === 16 && isNoteOn && velocity > 0) {
        /* Save current set before going to set view */
        if (state.currentSet >= 0) {
            markDirty();
        }
        /* Transition to set view */
        modes[state.trackMode].onExit();
        enterSetView();
        setView.onEnter();
        setView.updateLEDs();
        return true;
    }

    /* Shift + Step 2 enters channel mode */
    if (state.trackMode === 'normal' && state.shiftHeld && isNote && note === 17 && isNoteOn && velocity > 0) {
        modes.normal.onExit();
        enterChannelMode();
        modes.channel.onEnter();
        updateLEDs();
        return true;
    }

    /* Shift + Step 5 enters speed mode */
    if (state.trackMode === 'normal' && state.shiftHeld && isNote && note === 20 && isNoteOn && velocity > 0) {
        modes.normal.onExit();
        enterSpeedMode();
        modes.speed.onEnter();
        updateLEDs();
        return true;
    }

    /* Shift + Step 7 enters swing mode */
    if (state.trackMode === 'normal' && state.shiftHeld && isNote && note === 22 && isNoteOn && velocity > 0) {
        modes.normal.onExit();
        enterSwingMode();
        modes.swing.onEnter();
        updateLEDs();
        return true;
    }

    /* Shift + Step 8 toggles transpose (chordFollow) for current track */
    if (state.trackMode === 'normal' && state.shiftHeld && isNote && note === 23 && isNoteOn && velocity > 0) {
        state.chordFollow[state.currentTrack] = !state.chordFollow[state.currentTrack];
        setParam(`track_${state.currentTrack}_chord_follow`, state.chordFollow[state.currentTrack] ? "1" : "0");
        markDirty();
        const status = state.chordFollow[state.currentTrack] ? "ON" : "OFF";
        displayMessage(`Track ${state.currentTrack + 1}`, `Transpose: ${status}`, "", "");
        updateLEDs();
        return true;
    }

    /* Shift + Step 11 enters arp mode */
    if (state.trackMode === 'normal' && state.shiftHeld && isNote && note === 26 && isNoteOn && velocity > 0) {
        modes.normal.onExit();
        enterArpMode();
        modes.arp.onEnter();
        updateLEDs();
        return true;
    }

    /* Loop button press enters loop mode */
    if (state.trackMode === 'normal' && isCC && note === MoveLoop && velocity > 0) {
        modes.normal.onExit();
        enterLoopEdit();
        modes.loop.onEnter();
        updateLEDs();
        return true;
    }

    /* Capture + step enters spark mode */
    if (state.trackMode === 'normal' && state.captureHeld && isNote && note >= 16 && note <= 31 && isNoteOn && velocity > 0) {
        modes.normal.onExit();
        const stepIdx = note - 16;
        enterSparkMode();
        state.sparkSelectedSteps.add(stepIdx);
        modes.spark.onEnter();
        updateLEDs();
        return true;
    }

    /*
     * Mode EXIT and CROSS-MODE transitions for channel/speed/swing/arp
     * - Jog click or back button: exit to normal
     * - Shift+same step: exit to normal
     * - Shift+different step: switch directly to that mode
     */
    if (state.trackMode === 'channel' || state.trackMode === 'speed' || state.trackMode === 'swing' || state.trackMode === 'arp') {
        const isJogClick = isNote && note === MoveMainButton && isNoteOn && velocity > 0;
        const isBackButton = isCC && note === MoveBack && velocity > 0;

        /* Check for shift+step presses */
        const isShiftStep = state.shiftHeld && isNote && isNoteOn && velocity > 0;
        const isChannelStep = isShiftStep && note === 17;
        const isSpeedStep = isShiftStep && note === 20;
        const isSwingStep = isShiftStep && note === 22;
        const isArpStep = isShiftStep && note === 26;

        /* Exit current mode helper */
        const exitCurrentMode = () => {
            modes[state.trackMode].onExit();
            if (state.trackMode === 'channel') exitChannelMode();
            else if (state.trackMode === 'speed') exitSpeedMode();
            else if (state.trackMode === 'swing') exitSwingMode();
            else exitArpMode();
        };

        /* Jog click or back: exit to normal */
        if (isJogClick || isBackButton) {
            exitCurrentMode();
            modes.normal.onEnter();
            updateLEDs();
            return true;
        }

        /* Shift+same step: exit to normal */
        if ((state.trackMode === 'channel' && isChannelStep) ||
            (state.trackMode === 'speed' && isSpeedStep) ||
            (state.trackMode === 'swing' && isSwingStep) ||
            (state.trackMode === 'arp' && isArpStep)) {
            exitCurrentMode();
            modes.normal.onEnter();
            updateLEDs();
            return true;
        }

        /* Shift+different step: cross-mode transition */
        if (isChannelStep && state.trackMode !== 'channel') {
            exitCurrentMode();
            enterChannelMode();
            modes.channel.onEnter();
            updateLEDs();
            return true;
        }
        if (isSpeedStep && state.trackMode !== 'speed') {
            exitCurrentMode();
            enterSpeedMode();
            modes.speed.onEnter();
            updateLEDs();
            return true;
        }
        if (isSwingStep && state.trackMode !== 'swing') {
            exitCurrentMode();
            enterSwingMode();
            modes.swing.onEnter();
            updateLEDs();
            return true;
        }
        if (isArpStep && state.trackMode !== 'arp') {
            exitCurrentMode();
            enterArpMode();
            modes.arp.onEnter();
            updateLEDs();
            return true;
        }
    }

    /* Loop button release exits loop mode */
    if (state.trackMode === 'loop' && isCC && note === MoveLoop && velocity === 0) {
        modes.loop.onExit();
        exitLoopEdit();
        modes.normal.onEnter();
        updateLEDs();
        return true;
    }

    /* Capture button toggles spark mode */
    if (isCC && note === MoveCapture) {
        if (velocity > 0) {
            if (state.trackMode === 'spark') {
                modes.spark.onExit();
                exitSparkMode();
                modes.normal.onEnter();
                updateLEDs();
                return true;
            } else if (state.trackMode === 'normal') {
                state.captureHeld = true;
                updateLEDs();  /* Show spark preview */
            }
        } else {
            state.captureHeld = false;
            if (state.trackMode === 'normal') {
                updateLEDs();  /* Restore normal view */
            }
        }
        return true;
    }

    /*
     * Delegate ALL other input to current mode
     * Mode handles its own track buttons, knobs, steps, pads, jog wheel, etc.
     */
    return modes[state.trackMode].onInput(data);
}

/**
 * Update all LEDs for track view
 * Coordinator owns pads, delegates other LEDs to sub-mode
 */
export function updateLEDs() {
    updatePadLEDs();  // View-level pad control
    modes[state.trackMode].updateLEDs();  // Sub-mode handles rest
}


/**
 * Update display content for track view
 */
export function updateDisplayContent() {
    modes[state.trackMode].updateDisplayContent();
}

/**
 * Lightweight playhead update - only updates the two step LEDs that changed
 * Call this from tick() instead of full updateLEDs()
 */
export function updatePlayhead(oldStep, newStep) {
    if (modes[state.trackMode].updatePlayhead) {
        modes[state.trackMode].updatePlayhead(oldStep, newStep);
    }
}

/**
 * Called from main tick() to check for display auto-return
 */
export function tick() {
    if (state.trackMode === 'normal') {
        if (normal.checkDisplayReturn) {
            normal.checkDisplayReturn();
        }
        if (normal.checkCopyHold) {
            normal.checkCopyHold();
        }
    }
}
