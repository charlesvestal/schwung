/*
 * Sampler & Skipback overlay rendering for Shadow UI.
 *
 * The shim publishes state via /move-shadow-overlay SHM; this module
 * reads that state (passed in from shadow_ui.js) and draws the overlays
 * using the same TTF font and layout helpers as the rest of Shadow UI.
 */

import { drawRect } from '/data/UserData/move-anything/shared/menu_layout.mjs';

const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const SAMPLE_RATE = 44100;

/* Overlay type constants (must match shadow_constants.h) */
export const OVERLAY_NONE = 0;
export const OVERLAY_SAMPLER = 1;
export const OVERLAY_SKIPBACK = 2;
export const OVERLAY_SHIFT_KNOB = 3;

/* Sampler state constants */
const SAMPLER_IDLE = 0;
const SAMPLER_ARMED = 1;
const SAMPLER_RECORDING = 2;

/* Recording title flash state */
let recordingFlashCounter = 0;

/**
 * Draw a VU meter bar with border and log-scale fill.
 * @param {number} x - Left edge
 * @param {number} y - Top edge
 * @param {number} w - Width
 * @param {number} h - Height
 * @param {number} peak - Raw int16 peak (0-32767)
 */
export function drawVuMeter(x, y, w, h, peak) {
    /* Border */
    drawRect(x, y, w, h, 1);

    /* Log scale: map -48dB..0dB to 0..bar width */
    let vuNorm = 0;
    if (peak > 0) {
        const db = 20 * Math.log10(peak / 32767);
        vuNorm = (db + 48) / 48;
        if (vuNorm < 0) vuNorm = 0;
        if (vuNorm > 1) vuNorm = 1;
    }
    let fillW = Math.floor(vuNorm * (w - 2));
    if (fillW > w - 2) fillW = w - 2;
    if (fillW > 0) {
        fill_rect(x + 1, y + 1, fillW, h - 2, 1);
    }
}

/**
 * Draw the sampler armed screen (source/duration selection + VU).
 */
export function drawSamplerArmed(state) {
    clear_screen();

    /* Title */
    const title = "QUANTIZED SAMPLER";
    const titleX = Math.floor((SCREEN_WIDTH - title.length * 6) / 2);
    print(titleX, 0, title, 1);

    /* Source line */
    const cursor = state.samplerCursor;
    const sourceLabel = state.samplerSource === 0 ? "Resample" : "Move Input";
    const srcPrefix = cursor === 0 ? ">" : " ";
    print(0, 16, srcPrefix + "Source: " + sourceLabel, 1);

    /* Duration line */
    const bars = state.samplerDurationBars;
    const durPrefix = cursor === 1 ? ">" : " ";
    if (bars === 0) {
        print(0, 24, durPrefix + "Dur: Until stop", 1);
    } else {
        print(0, 24, durPrefix + "Dur: " + bars + " bar" + (bars > 1 ? "s" : ""), 1);
    }

    /* VU meter */
    drawVuMeter(4, 44, 120, 5, state.samplerVuPeak);

    /* Instructions */
    print(0, 52, "Play/Note to record", 1);
}

/**
 * Draw the sampler recording screen (progress + VU).
 */
export function drawSamplerRecording(state) {
    clear_screen();

    /* Flashing title (~4Hz at ~57fps = toggle every 14 frames) */
    recordingFlashCounter = (recordingFlashCounter + 1) % 28;
    if (Math.floor(recordingFlashCounter / 14) === 0) {
        const title = "** RECORDING **";
        const titleX = Math.floor((SCREEN_WIDTH - title.length * 6) / 2);
        print(titleX, 0, title, 1);
    }

    /* Source (locked, no cursor) */
    const sourceLabel = state.samplerSource === 0 ? "Resample" : "Move Input";
    print(0, 16, " Source: " + sourceLabel, 1);

    /* Progress */
    const bars = state.samplerTargetBars;
    if (bars === 0) {
        const secs = (state.samplerSamplesWritten / SAMPLE_RATE).toFixed(1);
        print(0, 24, " Elapsed: " + secs + "s", 1);
    } else {
        let currentBar = state.samplerBarsCompleted + 1;
        if (currentBar > bars) currentBar = bars;
        print(0, 24, " Bar " + currentBar + " / " + bars, 1);
    }

    /* Progress bar (fixed duration only) */
    if (bars > 0) {
        const progX = 4, progY = 32, progW = 120, progH = 5;
        drawRect(progX, progY, progW, progH, 1);
        let progress = 0;
        if (state.samplerClockReceived && state.samplerTargetPulses > 0) {
            progress = state.samplerClockCount / state.samplerTargetPulses;
        } else if (state.samplerFallbackTarget > 0) {
            progress = state.samplerFallbackBlocks / state.samplerFallbackTarget;
        }
        if (progress > 1) progress = 1;
        const fillW = Math.floor((progW - 2) * progress);
        if (fillW > 0) {
            fill_rect(progX + 1, progY + 1, fillW, progH - 2, 1);
        }
    }

    /* VU meter */
    drawVuMeter(4, 44, 120, 5, state.samplerVuPeak);

    /* Instructions */
    print(0, 52, "Sample to stop", 1);
}

/**
 * Draw the "Sample saved!" confirmation.
 */
export function drawSamplerSaved() {
    clear_screen();
    const msg = "Sample saved!";
    const msgX = Math.floor((SCREEN_WIDTH - msg.length * 6) / 2);
    print(msgX, 24, msg, 1);
}

/**
 * Draw the "Skipback saved!" toast overlay.
 * Draws on top of the current display content.
 */
export function drawSkipbackToast() {
    const boxW = 110;
    const boxH = 20;
    const boxX = Math.floor((SCREEN_WIDTH - boxW) / 2);
    const boxY = Math.floor((SCREEN_HEIGHT - boxH) / 2);

    /* Background and border */
    fill_rect(boxX, boxY, boxW, boxH, 0);
    drawRect(boxX, boxY, boxW, boxH, 1);

    const msg = "Skipback saved!";
    const msgX = Math.floor((SCREEN_WIDTH - msg.length * 6) / 2);
    print(msgX, boxY + 7, msg, 1);
}

/**
 * Shift+knob overlay box dimensions (exported for rect blit coordinates).
 */
export const SHIFT_KNOB_BOX_W = 110;
export const SHIFT_KNOB_BOX_H = 38;
export const SHIFT_KNOB_BOX_X = Math.floor((SCREEN_WIDTH - SHIFT_KNOB_BOX_W) / 2);
export const SHIFT_KNOB_BOX_Y = Math.floor((SCREEN_HEIGHT - SHIFT_KNOB_BOX_H) / 2);

/**
 * Draw the shift+knob parameter overlay (patch, param name, value).
 */
export function drawShiftKnobOverlay(state) {
    const bx = SHIFT_KNOB_BOX_X;
    const by = SHIFT_KNOB_BOX_Y;
    const bw = SHIFT_KNOB_BOX_W;
    const bh = SHIFT_KNOB_BOX_H;

    /* Background and border */
    fill_rect(bx, by, bw, bh, 0);
    drawRect(bx, by, bw, bh, 1);

    /* Three lines of text */
    const tx = bx + 4;
    print(tx, by + 3, state.shiftKnobPatch || "", 1);
    print(tx, by + 14, state.shiftKnobParam || "", 1);
    print(tx, by + 25, state.shiftKnobValue || "", 1);
}

/**
 * Main overlay dispatch - call from tick() with current overlay state.
 * Returns true if an overlay was drawn (caller may skip normal view).
 *
 * @param {object} state - overlay state from shadow_get_overlay_state()
 * @returns {boolean} true if fullscreen overlay was drawn
 */
export function drawSamplerOverlay(state) {
    if (!state || state.type !== OVERLAY_SAMPLER) return false;

    if (state.samplerFullscreen) {
        if (state.samplerState === SAMPLER_ARMED) {
            drawSamplerArmed(state);
        } else if (state.samplerState === SAMPLER_RECORDING) {
            drawSamplerRecording(state);
        } else {
            /* IDLE with fullscreen = "saved" message */
            drawSamplerSaved();
        }
        return true;
    }
    return false;
}

/**
 * Reset flash counter (call when sampler transitions to recording).
 */
export function resetFlashCounter() {
    recordingFlashCounter = 0;
}
