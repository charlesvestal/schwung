/*
 * Print Stems — Tool for bouncing populated clips to a sibling audio-clip set.
 *
 * Reads per-track pre-MFX audio from the shim's /schwung-print-capture ring
 * (host_print_capture_read), fires pads via move_midi_inject_to_move(), and
 * records each populated clip to its own stereo WAV, packaged as a sibling
 * "<setname> Stems" Move audio-clip set.
 *
 * skip_led_clear: true — Move's set overview pad colors stay visible.
 *
 * Phase 3 status: skeleton + capture-read plumbing. The capture state machine,
 * multi-pass orchestration, and output assembly land in later phases.
 */

import {
    MidiCC, MoveMainButton, MoveBack
} from '/data/UserData/schwung/shared/constants.mjs';

import {
    isCapacitiveTouchMessage
} from '/data/UserData/schwung/shared/input_filter.mjs';

import {
    drawMessageOverlay
} from '/data/UserData/schwung/shared/menu_layout.mjs';

import {
    announceView, announceOverlay
} from '/data/UserData/schwung/shared/screen_reader.mjs';

/* ── Constants ─────────────────────────────────────────────────────── */

const NUM_TRACKS = 4;
const FRAMES_PER_BLOCK = 128;          /* matches PRINT_CAPTURE_FRAMES_PER_BLOCK */
const RING_BLOCKS = 64;                /* matches PRINT_CAPTURE_RING_BLOCKS */
const SAMPLES_PER_BLOCK = FRAMES_PER_BLOCK * 2;   /* interleaved stereo */

/* RMS below this (int16 scale) counts as silence. ~-60 dBFS ≈ 32. */
const SILENCE_RMS_THRESHOLD = 30;

/* ── Capture read helpers (Task 3.2) ───────────────────────────────── */

/* Most-recent `blockCount` blocks for `track` as an Int16Array, or null. */
function readRecentAudio(track, blockCount) {
    if (typeof host_print_capture_read !== "function") return null;
    return host_print_capture_read(track, blockCount);
}

/* Producer's monotonic block counter (lets JS pace itself), or 0. */
function captureWriteIndex() {
    if (typeof host_print_capture_write_index !== "function") return 0;
    return host_print_capture_write_index();
}

function rmsOfBlock(int16Array) {
    if (!int16Array || int16Array.length === 0) return 0;
    let sum = 0;
    for (let i = 0; i < int16Array.length; i++) {
        sum += int16Array[i] * int16Array[i];
    }
    return Math.sqrt(sum / int16Array.length);
}

/* True if all 4 tracks are below the silence threshold over the last
 * `windowBlocks` blocks. Returns false if the ring isn't advancing. */
function allTracksSilent(threshold = SILENCE_RMS_THRESHOLD, windowBlocks = 4) {
    for (let t = 0; t < NUM_TRACKS; t++) {
        const buf = readRecentAudio(t, windowBlocks);
        if (!buf) return false;
        if (rmsOfBlock(buf) > threshold) return false;
    }
    return true;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

globalThis.init = function() {
    console.log("print-stems: init");
    if (typeof host_print_capture_read === "function" &&
        typeof host_print_capture_write_index === "function") {
        console.log("print-stems: capture API present");
    } else {
        console.log("print-stems: WARNING capture API missing");
    }
    announceView("Print Stems");
};

globalThis.tick = function() {
    clear_screen();
    drawMessageOverlay("Print Stems", ["Phase 3 WIP", "Back: exit"], false);
};

globalThis.onMidiMessageInternal = function(data) {
    if (isCapacitiveTouchMessage(data)) return;

    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    if (status === MidiCC && d1 === MoveBack && d2 > 0) {
        if (typeof host_exit_module === "function") host_exit_module();
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {};
