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
 * Phase 3 status: skeleton + capture-read plumbing + pad-firing primitives.
 * The capture state machine, multi-pass orchestration, and output assembly
 * land in later phases.
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

import {
    parseClipGrid
} from '/data/UserData/schwung/modules/tools/print-stems/abl_io.mjs';

/* ── Constants ─────────────────────────────────────────────────────── */

const NUM_TRACKS = 4;
const NUM_COLS = 8;
const COL_LETTERS = "ABCDEFGH";
const SETS_DIR = "/data/UserData/UserLibrary/Sets";

const FRAMES_PER_BLOCK = 128;          /* matches PRINT_CAPTURE_FRAMES_PER_BLOCK */
const RING_BLOCKS = 64;                /* matches PRINT_CAPTURE_RING_BLOCKS */
const SAMPLES_PER_BLOCK = FRAMES_PER_BLOCK * 2;   /* interleaved stereo */

/* RMS below this (int16 scale) counts as silence. ~-60 dBFS ≈ 32. */
const SILENCE_RMS_THRESHOLD = 30;

/* Pad-injection cadence (mirrors song-mode). Note-offs are deferred several
 * ticks after their note-on — sending on+off in the same MIDI_IN frame makes
 * Move ignore the press. */
const INJECT_INTERVAL_MS = 50;
const NOTE_OFF_DEFER_TICKS = 10;

/* ── State ─────────────────────────────────────────────────────────── */

let grid = null;                       /* parseClipGrid() result, or null */
let tempo = 120;
let barDurationMs = 2000;
let setName = "";
let activeSetUuid = "";
/* Per-track first empty column (a "silence pad" to halt that track), or null. */
let silencePads = [null, null, null, null];

/* MIDI inject queue — drained one packet per tick at INJECT_INTERVAL_MS. */
let injectQueue = [];
let pendingNoteOffs = [];
let lastInjectTime = 0;

/* ── Pad helpers ───────────────────────────────────────────────────── */

/* Pad note for track t (0-3, top=0), column c (0-7). */
function padNote(t, c) { return (92 - 8 * t) + c; }

/* Pad note that silences track t (its first empty column), or null. */
function silencePadNote(t) {
    const c = silencePads[t];
    return c === null ? null : padNote(t, c);
}

/* ── Set loading ───────────────────────────────────────────────────── */

function findSongAbl(uuid, name) {
    const directPath = SETS_DIR + "/" + uuid + "/" + name + "/Song.abl";
    if (host_file_exists(directPath)) return directPath;
    return null;
}

/* Load active_set.txt → find Song.abl → parse grid. Returns true on success. */
function loadActiveSetGrid() {
    const raw = host_read_file("/data/UserData/schwung/active_set.txt");
    if (!raw) { console.log("print-stems: no active_set.txt"); return false; }
    const lines = raw.split("\n");
    const uuid = lines[0] ? lines[0].trim() : "";
    setName = lines[1] ? lines[1].trim() : "";
    if (!uuid || !setName) { console.log("print-stems: incomplete active_set.txt"); return false; }
    activeSetUuid = uuid;

    const songPath = findSongAbl(uuid, setName);
    if (!songPath) { console.log("print-stems: Song.abl not found for " + setName); return false; }

    const content = host_read_file(songPath);
    if (!content) { console.log("print-stems: failed to read " + songPath); return false; }

    try {
        const song = JSON.parse(content);
        grid = parseClipGrid(song);
        if (typeof grid.tempo === "number" && grid.tempo > 0) {
            tempo = grid.tempo;
            barDurationMs = (60000 / tempo) * 4;
        }
        computeSilencePads();
        return true;
    } catch (e) {
        console.log("print-stems: JSON parse error: " + e);
        return false;
    }
}

/* First empty column per track = a pad we can fire to silence that track. */
function computeSilencePads() {
    for (let t = 0; t < NUM_TRACKS; t++) {
        silencePads[t] = null;
        if (!grid) continue;
        for (let c = 0; c < NUM_COLS; c++) {
            if (!grid.cells[t][c].exists) { silencePads[t] = c; break; }
        }
    }
}

function populatedClipCount() {
    if (!grid) return 0;
    let n = 0;
    for (let t = 0; t < NUM_TRACKS; t++)
        for (let c = 0; c < NUM_COLS; c++)
            if (grid.cells[t][c].exists) n++;
    return n;
}

/* ── Pad firing (inject queue) ─────────────────────────────────────── */

function queueInjectNote(note, velocity) {
    /* USB-MIDI cable 0: NoteOn = CIN 0x09/status 0x90; NoteOff = 0x08/0x80. */
    const status = velocity > 0 ? 0x90 : 0x80;
    const cin = velocity > 0 ? 0x09 : 0x08;
    injectQueue.push([cin, status, note, velocity]);
}

/* Fire a column across all tracks: the clip pad where one exists, else the
 * track's silence pad (so empty tracks are forced silent rather than left
 * ringing a previous clip). */
function fireColumn(col) {
    if (!grid) return;
    const fired = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        let note = null;
        if (grid.cells[t][col].exists) note = padNote(t, col);
        else note = silencePadNote(t);
        if (note === null) continue;
        queueInjectNote(note, 127);
        pendingNoteOffs.push({ note: note, ticks: NOTE_OFF_DEFER_TICKS });
        fired.push(note);
    }
    console.log("print-stems: fireColumn " + COL_LETTERS[col] + " notes=[" + fired.join(",") + "]");
}

/* Halt every track by firing its silence pad. */
function stopAllTracks() {
    const fired = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        const note = silencePadNote(t);
        if (note === null) continue;
        queueInjectNote(note, 127);
        pendingNoteOffs.push({ note: note, ticks: NOTE_OFF_DEFER_TICKS });
        fired.push(note);
    }
    console.log("print-stems: stopAllTracks notes=[" + fired.join(",") + "]");
}

/* Drain one queued packet per tick (throttled), then service deferred note-offs. */
function drainInjectQueue() {
    const now = Date.now();
    if (injectQueue.length > 0 && (now - lastInjectTime) >= INJECT_INTERVAL_MS) {
        const pkt = injectQueue.shift();
        move_midi_inject_to_move(pkt);
        lastInjectTime = now;
    }
    for (let i = pendingNoteOffs.length - 1; i >= 0; i--) {
        pendingNoteOffs[i].ticks--;
        if (pendingNoteOffs[i].ticks <= 0) {
            queueInjectNote(pendingNoteOffs[i].note, 0);
            pendingNoteOffs.splice(i, 1);
        }
    }
}

/* ── Capture read helpers (Task 3.2) ───────────────────────────────── */

function readRecentAudio(track, blockCount) {
    if (typeof host_print_capture_read !== "function") return null;
    return host_print_capture_read(track, blockCount);
}

function captureWriteIndex() {
    if (typeof host_print_capture_write_index !== "function") return 0;
    return host_print_capture_write_index();
}

function rmsOfBlock(int16Array) {
    if (!int16Array || int16Array.length === 0) return 0;
    let sum = 0;
    for (let i = 0; i < int16Array.length; i++) sum += int16Array[i] * int16Array[i];
    return Math.sqrt(sum / int16Array.length);
}

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
    if (typeof move_midi_inject_to_move !== "function") {
        console.log("print-stems: WARNING move_midi_inject_to_move missing");
    }
    injectQueue = [];
    pendingNoteOffs = [];

    const ok = loadActiveSetGrid();
    if (ok) {
        console.log("print-stems: set '" + setName + "' tempo=" + tempo +
                    " clips=" + populatedClipCount());
        for (let t = 0; t < NUM_TRACKS; t++) {
            let row = "T" + (t + 1) + ":";
            for (let c = 0; c < NUM_COLS; c++) {
                const cell = grid.cells[t][c];
                row += cell.exists ? " " + COL_LETTERS[c] + "(" + cell.beats + "b)" : " --";
            }
            console.log("print-stems: " + row);
        }
        console.log("print-stems: silence pads=" + silencePads.map(
            (c, t) => c !== null ? (t + 1) + COL_LETTERS[c] : (t + 1) + "?").join(" "));
        announceView("Print Stems, " + setName);
    } else {
        announceView("Print Stems, no active set");
    }
};

globalThis.tick = function() {
    drainInjectQueue();

    clear_screen();
    if (!grid) {
        drawMessageOverlay("Print Stems", ["No active set", "Back: exit"], false);
        return;
    }
    drawMessageOverlay("Print Stems", [
        setName.length > 18 ? setName.slice(0, 18) : setName,
        populatedClipCount() + " clips  Jog: fire A",
    ], false);
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

    /* Jog click: fire column A across all tracks (Task 3.3 manual test). */
    if (status === MidiCC && d1 === MoveMainButton && d2 > 0) {
        fireColumn(0);
        announceOverlay("Fire column A");
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {};
