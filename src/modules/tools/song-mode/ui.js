/*
 * Song Mode — Tool for sequencing clips across time.
 *
 * Reads the current set's Song.abl to discover clip layout and tempo,
 * then lets the user build an ordered song from pad assignments.
 * Playback triggers pads via move_midi_inject_to_move() and advances
 * automatically based on wall-clock timing.
 *
 * skip_led_clear: true — Move's set overview pad colors stay visible.
 */

import {
    MidiNoteOn, MidiCC,
    MoveMainKnob, MoveMainButton, MoveBack, MoveShift,
    MovePlay, MoveRec, MoveDelete, MoveCopy, MoveUndo, MoveLoop,
    MoveUp, MoveDown, MoveLeft, MoveRight,
    MoveStep1, MoveStep16,
    Black, White, BrightRed, BrightGreen
} from '/data/UserData/move-anything/shared/constants.mjs';

import {
    isCapacitiveTouchMessage, decodeDelta
} from '/data/UserData/move-anything/shared/input_filter.mjs';

import {
    drawMessageOverlay
} from '/data/UserData/move-anything/shared/menu_layout.mjs';

import {
    announce, announceMenuItem, announceParameter, announceView, announceOverlay
} from '/data/UserData/move-anything/shared/screen_reader.mjs';

/* ── Constants ─────────────────────────────────────────────────────── */

const NUM_TRACKS = 4;
const NUM_COLS   = 8;
const SETS_DIR   = "/data/UserData/UserLibrary/Sets";

/* Pad note for track t (0-3, top=0) column c (0-7) */
function padNote(t, c) { return (92 - 8 * t) + c; }

/* Reverse: note → {track, col} or null */
function noteToGrid(note) {
    if (note < 68 || note > 99) return null;
    const idx = note - 68;
    const row = 3 - Math.floor(idx / 8);
    const col = idx % 8;
    return { track: row, col: col };
}

/* Column labels */
const COL_LABELS = "ABCDEFGH";

/* ── State ─────────────────────────────────────────────────────────── */

/* Clip grid parsed from Song.abl */
let clipGrid = [];
let tempo = 120;
let barDurationMs = 2000;
let setName = "";
let activeSetUuid = "";     /* UUID of current set (for persistence) */

/* Per-track empty slot column index (for silencing tracks before playback) */
let silencePads = [null, null, null, null];

/* Song arrangement — always ends with one empty row */
let songEntries = [];
let selectedEntry = 0;
let scrollOffset = 0;

/* Playback */
let playbackState = "stopped";
let currentEntryIndex = 0;
let playbackStartEntry = 0;  /* entry to return to on stop */
let playStartTime = 0;
let nextEntryQueued = false;
let playStartPending = 0;    /* startup phase: 0=off, 1=selecting, 2=stopping, 3=restarting */
let playStartPhaseDelay = 0; /* ticks to wait between phases */
const PHASE_DELAY_TICKS = 3; /* ~50ms between phases */
let loopEnabled = false;     /* loop song from beginning when it ends */

/* Undo */
let undoStack = [];          /* snapshots of songEntries for undo */
const MAX_UNDO = 20;

/* MIDI inject queue — drain packets per tick */
let injectQueue = [];

/* Deferred note-offs: [note, ticksRemaining] */
let pendingNoteOffs = [];

/* Shift tracking */
let shiftHeld = false;

/* Step button navigation */
const STEPS_PER_PAGE = 16;
let stepPage = 0;           /* 0 = entries 0-15, 1 = entries 16-31, etc. */
let lastStepLedKey = "";    /* change detection for step LED updates */
let stepLedQueue = [];      /* staggered step LED packets to drain per tick */
const STEP_LEDS_PER_TICK = 4; /* drain 4 step LED packets per tick */

/* Step hold detection */
let heldStepNote = -1;      /* note of currently held step button, -1 = none */
let heldStepTime = 0;       /* Date.now() when step was pressed */
let heldStepHandled = false; /* true if hold already opened step params */
const STEP_HOLD_MS = 300;   /* ms before hold opens step params */

/* Pad LED highlighting */
const LED_RED = BrightRed;
let padLedSnapshot = {};    /* note -> original color from Move (init time) */
let highlightedNotes = [];  /* currently red-highlighted pad notes */
let lastHighlightKey = "";  /* change detection */
let restoreRetryQueue = [];   /* [{note, ttl}, ...] — pads needing repeated restore */
let ledRetryQueue = [];       /* [0x09, 0x90, note, color] packets to drain 1/tick */

/* Play button LED */
let playButtonLit = false;

/* Recording */
let recording = false;
let recordStartTime = 0;       /* when recording began (ms) */
let recordStopTime = 0;        /* auto-stop timestamp (ms), 0 = no auto-stop pending */
let recordLedLit = false;
const RECORDINGS_DIR = "/data/UserData/Recordings/Song Mode";
let recordingSavedUntil = 0;   /* show "Recording saved!" overlay until this timestamp */
let recordingSavedAnnounced = false;
let overlayMessage = "";       /* generic overlay text */
let overlayTimeout = 0;        /* show overlay until this timestamp */


/* Step parameter editing */
const REPEAT_VALUES = [1, 2, 4, 8, 16, 32, 64];
let currentView = "list";   /* "list" or "step_params" */
let stepParamsPeek = false; /* true = entered via hold (exit on release), false = shift (sticky) */
let sessionWarning = false;  /* true = show "switch to session" screen, dismiss with jog click */
let editingEntry = -1;      /* index of entry being edited */

/* Persistence */
const SET_STATE_DIR = "/data/UserData/move-anything/set_state";
let lastSavedJson = "";     /* JSON snapshot for dirty detection */

/* ── Song.abl Parsing ──────────────────────────────────────────────── */

function loadSetData() {
    const raw = host_read_file("/data/UserData/move-anything/active_set.txt");
    if (!raw) {
        console.log("song-mode: no active_set.txt");
        return false;
    }
    const lines = raw.split("\n");
    const uuid = lines[0] ? lines[0].trim() : "";
    setName = lines[1] ? lines[1].trim() : "";
    if (!uuid || !setName) {
        console.log("song-mode: incomplete active_set.txt");
        return false;
    }
    activeSetUuid = uuid;

    const songPath = findSongAbl(uuid, setName);
    if (!songPath) {
        console.log("song-mode: Song.abl not found for " + setName);
        return false;
    }

    const content = host_read_file(songPath);
    if (!content) {
        console.log("song-mode: failed to read " + songPath);
        return false;
    }

    try {
        const song = JSON.parse(content);
        parseSong(song);
        return true;
    } catch (e) {
        console.log("song-mode: JSON parse error: " + e);
        return false;
    }
}

function findSongAbl(uuid, name) {
    const directPath = SETS_DIR + "/" + uuid + "/" + name + "/Song.abl";
    if (host_file_exists(directPath)) return directPath;
    return null;
}

/* ── Persistence ─────────────────────────────────────────────────────── */

function songStatePath() {
    if (!activeSetUuid) return null;
    return SET_STATE_DIR + "/" + activeSetUuid + "/song_mode.json";
}

/* Strip trailing empty entries for clean serialization */
function serializableSongEntries() {
    const entries = [];
    for (const e of songEntries) {
        if (!isEntryEmpty(e)) entries.push({ pads: e.pads, repeats: e.repeats });
    }
    return entries;
}

function saveSongData() {
    const path = songStatePath();
    if (!path) return;
    const json = JSON.stringify(serializableSongEntries());
    if (json === lastSavedJson) return;  /* no change */
    host_write_file(path, json);
    lastSavedJson = json;
}

function loadSongData() {
    const path = songStatePath();
    if (!path) return false;
    const raw = host_read_file(path);
    if (!raw) return false;
    try {
        const entries = JSON.parse(raw);
        if (!Array.isArray(entries) || entries.length === 0) return false;
        songEntries = entries.map(e => ({
            pads: Array.isArray(e.pads) ? e.pads : [null, null, null, null],
            repeats: typeof e.repeats === "number" ? e.repeats : 1
        }));
        ensureTrailingEmpty();
        lastSavedJson = JSON.stringify(serializableSongEntries());
        console.log("song-mode: loaded " + entries.length + " entries from " + path);
        return true;
    } catch (e) {
        console.log("song-mode: failed to parse song_mode.json: " + e);
        return false;
    }
}

/* ── Undo ─────────────────────────────────────────────────────────── */

function pushUndo() {
    const snapshot = songEntries.map(e => ({ pads: [...e.pads], repeats: e.repeats }));
    undoStack.push(JSON.stringify(snapshot));
    if (undoStack.length > MAX_UNDO) undoStack.shift();
}

function popUndo() {
    if (undoStack.length === 0) return false;
    const snapshot = JSON.parse(undoStack.pop());
    songEntries = snapshot.map(e => ({ pads: e.pads, repeats: e.repeats }));
    ensureTrailingEmpty();
    if (selectedEntry >= songEntries.length) selectedEntry = songEntries.length - 1;
    lastStepLedKey = "";
    lastHighlightKey = "";
    return true;
}

function parseSong(song) {
    clipGrid = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        clipGrid[t] = [];
        for (let c = 0; c < NUM_COLS; c++) {
            clipGrid[t][c] = { exists: false, bars: 0 };
        }
    }

    if (song.tempo) {
        tempo = song.tempo;
        barDurationMs = (60000 / tempo) * 4;
    }

    const tracks = song.tracks;
    if (!Array.isArray(tracks)) return;

    for (let t = 0; t < Math.min(tracks.length, NUM_TRACKS); t++) {
        const track = tracks[t];
        if (!track || !track.clipSlots) continue;
        const slots = track.clipSlots;

        for (let c = 0; c < Math.min(slots.length, NUM_COLS); c++) {
            const slot = slots[c];
            if (!slot || !slot.clip) continue;
            const region = slot.clip.region;
            let beats = 0;
            if (region) {
                const loop = region.loop;
                if (loop && loop.isEnabled && loop.end > loop.start) {
                    /* Use loop range — this is what actually plays */
                    beats = loop.end - loop.start;
                } else {
                    /* No loop: use full region */
                    beats = (region.end || 0) - (region.start || 0);
                }
            }
            clipGrid[t][c] = { exists: true, bars: beats > 0 ? beats / 4 : 0 };
        }
    }
}

/* ── Entry Helpers ─────────────────────────────────────────────────── */

function entryBars(entry) {
    let maxBars = 0;
    for (let t = 0; t < NUM_TRACKS; t++) {
        const c = entry.pads[t];
        if (c !== null && clipGrid[t] && clipGrid[t][c] && clipGrid[t][c].exists) {
            maxBars = Math.max(maxBars, clipGrid[t][c].bars);
        }
    }
    return Math.max(maxBars, 1);
}

function entryLabel(entry) {
    let parts = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        const c = entry.pads[t];
        parts.push(c !== null ? (t + 1) + COL_LABELS[c] : "--");
    }
    return parts.join(" ");
}

function describeEntry(idx) {
    if (idx === songEntries.length) return "Play Song";
    if (idx === songEntries.length + 1) return "Record Song";
    if (idx >= songEntries.length) return "";
    const entry = songEntries[idx];
    if (isEntryEmpty(entry)) return "Step " + (idx + 1) + ", empty";
    const label = entryLabel(entry);
    const rep = entry.repeats > 1 ? ", repeat " + entry.repeats + " times" : "";
    return "Step " + (idx + 1) + ", " + label + rep;
}

function isEntryEmpty(entry) {
    return entry.pads.every(p => p === null);
}

function isEntryComplete(entry) {
    return entry.pads.every(p => p !== null);
}

function newEntry() {
    return { pads: [null, null, null, null], repeats: 1 };
}


/* Ensure there's always exactly one empty row at the end */
function ensureTrailingEmpty() {
    if (songEntries.length === 0 || !isEntryEmpty(songEntries[songEntries.length - 1])) {
        songEntries.push(newEntry());
    }
    /* Remove extra trailing empties */
    while (songEntries.length > 1 &&
           isEntryEmpty(songEntries[songEntries.length - 1]) &&
           isEntryEmpty(songEntries[songEntries.length - 2])) {
        songEntries.pop();
    }
}

/* ── Display ───────────────────────────────────────────────────────── */

const MAX_VISIBLE = 4;

function drawListView() {
    clear_screen();

    /* Header */
    const recFlash = recording && (Math.floor(Date.now() / 500) % 2 === 0);
    const loopStr = loopEnabled ? " [L]" : "";
    const titleStr = recFlash ? "Song REC" + loopStr : "Song Mode" + loopStr;
    print(2, 2, titleStr, 1);
    const pageLabel = stepPage > 0 ? "P" + (stepPage + 1) + " " : "";
    const nameStr = pageLabel + (setName || "No Set");
    const nameW = nameStr.length * 6;
    print(Math.max(128 - nameW - 2, 60), 2, nameStr, 1);
    fill_rect(0, 12, 128, 1, 1);

    /* Total items: songEntries + "Play Song" + "Record Song" at end */
    const totalItems = songEntries.length + 2;

    /* Ensure selected is in bounds */
    if (selectedEntry >= totalItems) selectedEntry = totalItems - 1;
    if (selectedEntry < 0) selectedEntry = 0;

    /* Scroll — follow playhead during playback, otherwise follow selection */
    const scrollTarget = playbackState === "playing" ? currentEntryIndex : selectedEntry;
    if (scrollTarget < scrollOffset) scrollOffset = scrollTarget;
    if (scrollTarget >= scrollOffset + MAX_VISIBLE) scrollOffset = scrollTarget - MAX_VISIBLE + 1;

    for (let i = 0; i < MAX_VISIBLE; i++) {
        const idx = scrollOffset + i;
        if (idx >= totalItems) break;

        const y = 15 + i * 9;
        const isSelected = idx === selectedEntry;

        if (isSelected) {
            fill_rect(0, y - 1, 128, 9, 1);
        }
        const color = isSelected ? 0 : 1;

        /* "Play Song" and "Record Song" items at end */
        if (idx === songEntries.length) {
            const playLabel = playbackState === "playing" ? ">> Stop Song" : ">> Play Song";
            print(2, y, playLabel, color);
            continue;
        }
        if (idx === songEntries.length + 1) {
            const recLabel = recording ? ">> Stop Recording" : ">> Record Song";
            print(2, y, recLabel, color);
            continue;
        }

        const entry = songEntries[idx];
        const isPlaying = playbackState === "playing" && idx === currentEntryIndex;
        const empty = isEntryEmpty(entry);

        const numStr = String(idx + 1).padStart(2, "0");
        const prefix = isPlaying ? ">" : " ";

        if (empty) {
            print(2, y, prefix + numStr + ": (empty)", color);
        } else {
            print(2, y, prefix + numStr + ":", color);
            print(26, y, entryLabel(entry), color);
            const repStr = "x" + entry.repeats;
            print(128 - repStr.length * 6 - 2, y, repStr, color);
        }
    }

    /* Scroll indicators */
    if (scrollOffset > 0) print(122, 13, "^", 1);
    if (scrollOffset + MAX_VISIBLE < totalItems) print(122, 52, "v", 1);

    /* Footer */
    fill_rect(0, 55, 128, 1, 1);
    if (recording) {
        const elapsed = (Date.now() - recordStartTime) / 1000;
        const mins = Math.floor(elapsed / 60);
        const secs = Math.floor(elapsed % 60);
        const timeStr = String(mins).padStart(2, "0") + ":" + String(secs).padStart(2, "0");
        const recLabel = (Math.floor(Date.now() / 500) % 2 === 0) ? "REC" : "   ";
        print(2, 57, recLabel + " " + timeStr + " step " + (currentEntryIndex + 1) + "/" + countNonEmpty(), 1);
    } else if (playbackState === "playing") {
        const entry = songEntries[currentEntryIndex];
        const totalBars = entryBars(entry) * entry.repeats;
        const elapsed = (Date.now() - playStartTime) / barDurationMs;
        const currentBar = Math.min(Math.floor(elapsed) + 1, totalBars);
        print(2, 57, "Playing " + (currentEntryIndex + 1) + "/" + countNonEmpty() + " bar " + currentBar + "/" + totalBars, 1);
    } else if (selectedEntry === songEntries.length) {
        print(2, 57, "Click:play  Shift:from top", 1);
    } else if (selectedEntry === songEntries.length + 1) {
        print(2, 57, "Click:record  Shift:from top", 1);
    } else {
        print(2, 57, "Pad:set X:del Copy:dup", 1);
    }
}

function countNonEmpty() {
    let n = 0;
    for (const e of songEntries) if (!isEntryEmpty(e)) n++;
    return n;
}

function drawStepParamsView() {
    clear_screen();

    const entry = songEntries[editingEntry];
    if (!entry) { currentView = "list"; return; }

    /* Header */
    const title = "Step " + String(editingEntry + 1).padStart(2, "0") + " Settings";
    print(2, 2, title, 1);
    fill_rect(0, 12, 128, 1, 1);

    /* Pad assignments */
    print(2, 15, entryLabel(entry), 1);

    /* Clip bars (informational) */
    const bars = entryBars(entry);
    print(2, 25, "Clip: " + bars + " bar" + (bars !== 1 ? "s" : ""), 1);

    /* Repeats — highlight value */
    print(2, 35, "Repeats:", 1);
    const repStr = " " + entry.repeats + "x ";
    fill_rect(50, 33, repStr.length * 6 + 2, 11, 1);
    print(52, 35, repStr, 0);

    /* Total duration */
    const total = bars * entry.repeats;
    print(2, 45, "= " + total + " bar" + (total !== 1 ? "s" : "") + " total", 1);

    /* Footer */
    fill_rect(0, 55, 128, 1, 1);
    print(2, 57, "Jog:change  Back:done", 1);
}

/* ── Playback Engine ───────────────────────────────────────────────── */

function startPlayback(fromBeginning) {
    let startIdx = -1;
    if (fromBeginning) {
        /* Shift+Play: find first complete entry */
        for (let i = 0; i < songEntries.length; i++) {
            if (!isEntryEmpty(songEntries[i]) && isEntryComplete(songEntries[i])) {
                startIdx = i;
                break;
            }
        }
    } else {
        /* Play: start from selected entry (or next complete one after it) */
        for (let i = selectedEntry; i < songEntries.length; i++) {
            if (!isEntryEmpty(songEntries[i]) && isEntryComplete(songEntries[i])) {
                startIdx = i;
                break;
            }
        }
    }
    if (startIdx < 0) {
        console.log("song-mode: no complete entries to play");
        overlayMessage = "No complete entries";
        overlayTimeout = Date.now() + 1500;
        announce("No complete entries");
        return;
    }

    playbackStartEntry = fromBeginning ? 0 : selectedEntry;
    console.log("song-mode: startPlayback from entry " + startIdx + (fromBeginning ? " (beginning)" : ""));
    currentEntryIndex = startIdx;

    /* Startup sequence:
     *   1. Mute Move's audio output
     *   2. Select clips via queued injection (one per tick)
     *   3. Play toggle (stop) → Play toggle (start)
     *   4. Unmute audio (in phase 3, after restart) */
    if (typeof host_mute_move_audio === "function") host_mute_move_audio(1);
    triggerEntry(currentEntryIndex);

    playStartPending = 1;
    playStartTime = 0;
    nextEntryQueued = false;
    playbackState = "playing";
}

function stopPlayback() {
    playbackState = "stopped";
    playStartPending = 0;
    playStartPhaseDelay = 0;
    injectQueue = [];
    pendingNoteOffs = [];
    /* Ensure audio is unmuted */
    if (typeof host_mute_move_audio === "function") host_mute_move_audio(0);
    /* Stop transport via Play CC toggle */
    queuePlayToggle();
    /* Return to the entry we started from */
    selectedEntry = playbackStartEntry;
    stepPage = Math.floor(selectedEntry / STEPS_PER_PAGE);
    lastStepLedKey = "";  /* force LED refresh */
    lastHighlightKey = "";  /* force pad highlight refresh */
    console.log("song-mode: stopPlayback, returning to entry " + selectedEntry);
}

function nextCompleteEntry(afterIdx) {
    for (let i = afterIdx + 1; i < songEntries.length; i++) {
        if (!isEntryEmpty(songEntries[i]) && isEntryComplete(songEntries[i])) return i;
    }
    return -1;
}

function firstCompleteEntry() {
    for (let i = 0; i < songEntries.length; i++) {
        if (!isEntryEmpty(songEntries[i]) && isEntryComplete(songEntries[i])) return i;
    }
    return -1;
}

function tickPlayback() {
    if (playbackState !== "playing") return;
    if (playStartPending > 0) return; /* waiting for clips to load before timing starts */
    if (currentEntryIndex >= songEntries.length) { stopPlayback(); return; }

    const entry = songEntries[currentEntryIndex];
    const totalBars = entryBars(entry) * entry.repeats;
    const elapsed = (Date.now() - playStartTime) / barDurationMs;

    /* Pre-trigger next entry 1 beat (0.25 bars) before end.
     * Move quantizes clip launches to bar boundaries, so selecting
     * the next clips just before the boundary makes them start on time. */
    let nextIdx = nextCompleteEntry(currentEntryIndex);
    /* If looping and no next entry, wrap to first */
    if (nextIdx < 0 && loopEnabled && !recording) {
        nextIdx = firstCompleteEntry();
    }
    if (!nextEntryQueued && elapsed >= totalBars - 0.25 && nextIdx >= 0) {
        console.log("song-mode: pre-trigger entry " + nextIdx + " at " + elapsed.toFixed(2) + "/" + totalBars + " bars");
        triggerEntry(nextIdx);
        nextEntryQueued = true;
    }

    /* Advance when current entry's duration has elapsed */
    if (elapsed >= totalBars) {
        if (nextIdx < 0) {
            console.log("song-mode: song ended");
            if (recording && recordStopTime === 0) {
                /* Recording: let transport run 1 extra bar to capture tail */
                recordStopTime = Date.now() + barDurationMs;
                console.log("song-mode: recording auto-stop scheduled in " + barDurationMs + "ms");
            } else {
                /* Not recording: stop immediately */
                stopPlayback();
            }
            return;
        }
        console.log("song-mode: advance to entry " + nextIdx);
        currentEntryIndex = nextIdx;
        playStartTime = Date.now();
        nextEntryQueued = false;
    }
}

function triggerEntry(entryIndex) {
    const entry = songEntries[entryIndex];
    if (!entry) return;

    const noteOns = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        const c = entry.pads[t];
        if (c !== null) noteOns.push(padNote(t, c));
    }

    console.log("song-mode: triggerEntry " + entryIndex + " notes=[" + noteOns.join(",") + "]");

    /* Only inject note-ons now. Note-offs are deferred by ~10 ticks
     * so Move sees them in separate MIDI_IN frames. Sending note-on
     * and note-off in the same frame can cause Move to ignore the press. */
    for (const note of noteOns) queueInjectNote(note, 127);
    for (const note of noteOns) pendingNoteOffs.push({ note: note, ticks: 10 });
}

/* ── MIDI Injection Queue ──────────────────────────────────────────── */

function queueInjectNote(note, velocity) {
    const status = velocity > 0 ? 0x90 : 0x80;
    const cin = velocity > 0 ? 0x09 : 0x08;
    injectQueue.push([cin, status, note, velocity]);
}

function queueInjectCC(cc, value) {
    injectQueue.push([0x0B, 0xB0, cc, value]);
}

/* Queue a Play CC press+release to toggle transport */
function queuePlayToggle() {
    queueInjectCC(MovePlay, 127);
    queueInjectCC(MovePlay, 0);
}

let lastInjectTime = 0;
const INJECT_INTERVAL_MS = 50; /* 50ms between packets — matches test tool timing */

function drainInjectQueue() {
    /* Drain 1 packet per call, throttled to INJECT_INTERVAL_MS minimum. */
    const now = Date.now();
    if (injectQueue.length > 0 && (now - lastInjectTime) >= INJECT_INTERVAL_MS) {
        const pkt = injectQueue.shift();
        console.log("song-mode: inject [" + pkt.join(",") + "] remaining=" + injectQueue.length);
        move_midi_inject_to_move(pkt);
        lastInjectTime = now;
    }

    /* Process deferred note-offs — count down and inject when ready */
    for (let i = pendingNoteOffs.length - 1; i >= 0; i--) {
        pendingNoteOffs[i].ticks--;
        if (pendingNoteOffs[i].ticks <= 0) {
            const note = pendingNoteOffs[i].note;
            queueInjectNote(note, 0);
            pendingNoteOffs.splice(i, 1);
        }
    }

    /* Startup state machine: select pads → Play CC (stop) → Play CC (start).
     * Each phase waits for the queue to drain, then pauses briefly. */
    if (playStartPending > 0 && injectQueue.length === 0 && pendingNoteOffs.length === 0) {
        if (playStartPhaseDelay > 0) {
            playStartPhaseDelay--;
            return;
        }
        if (playStartPending === 1) {
            /* Phase 1: pad selections drained. Toggle Play (stop transport). */
            queuePlayToggle();
            playStartPending = 2;
            playStartPhaseDelay = PHASE_DELAY_TICKS;
            console.log("song-mode: phase 1 done, toggling play (stop)");
        } else if (playStartPending === 2) {
            /* Phase 2: transport stopped. Toggle Play (restart). */
            queuePlayToggle();
            playStartPending = 3;
            playStartPhaseDelay = PHASE_DELAY_TICKS;
            console.log("song-mode: phase 2 done, toggling play (start)");
        } else if (playStartPending === 3) {
            /* Phase 3: playing with correct clips. Unmute and start timer. */
            if (typeof host_mute_move_audio === "function") host_mute_move_audio(0);
            playStartPending = 0;
            playStartTime = Date.now();
            console.log("song-mode: phase 3 done, timer started");
        }
    }
}

/* Immediately inject a single MIDI packet (for pad passthrough) */
function injectNow(cin, status, d1, d2) {
    move_midi_inject_to_move([cin, status, d1, d2]);
}

/* ── Step LED Management ──────────────────────────────────────────── */

/* Ensure songEntries has at least (idx + 1) entries */
function ensureEntryExists(idx) {
    while (songEntries.length <= idx) {
        songEntries.push(newEntry());
    }
}

/* Select entry by step button index (0-15) on current page.
 * Only selects song entries, not the Play/Record menu items. */
function selectStep(stepIdx) {
    const entryIdx = stepPage * STEPS_PER_PAGE + stepIdx;
    ensureEntryExists(entryIdx);
    ensureTrailingEmpty();
    /* Don't select past the last song entry (skip Play/Record items) */
    if (entryIdx > songEntries.length - 1) return;
    selectedEntry = entryIdx;
}

function getStepLedKey() {
    /* Encode page, selection, playback, and which entries are populated */
    let key = stepPage + ":" + selectedEntry + ":" + playbackState + ":" + currentEntryIndex;
    const base = stepPage * STEPS_PER_PAGE;
    for (let i = 0; i < STEPS_PER_PAGE; i++) {
        const idx = base + i;
        if (idx < songEntries.length && !isEntryEmpty(songEntries[idx])) {
            key += ":1";
        } else {
            key += ":0";
        }
    }
    return key;
}

function updateStepLeds() {
    const key = getStepLedKey();
    if (key === lastStepLedKey && stepLedQueue.length === 0) return;

    if (key !== lastStepLedKey) {
        lastStepLedKey = key;
        /* Build fresh queue — replaces any pending packets */
        stepLedQueue = [];
        const base = stepPage * STEPS_PER_PAGE;
        for (let i = 0; i < STEPS_PER_PAGE; i++) {
            const note = MoveStep1 + i;
            const entryIdx = base + i;
            let color = Black;

            if (playbackState === "playing" && entryIdx === currentEntryIndex) {
                color = BrightRed;    /* currently playing */
            } else if (entryIdx === selectedEntry && selectedEntry < songEntries.length) {
                color = White;        /* selected (not Play Song item) */
            } else if (entryIdx < songEntries.length && !isEntryEmpty(songEntries[entryIdx])) {
                color = BrightGreen;  /* populated */
            }

            stepLedQueue.push([0x09, 0x90, note, color]);
        }
    }

    /* Drain a few per tick to avoid buffer contention */
    for (let i = 0; i < STEP_LEDS_PER_TICK && stepLedQueue.length > 0; i++) {
        move_midi_internal_send(stepLedQueue.shift());
    }
}

function clearStepLeds() {
    stepLedQueue = [];
    for (let i = 0; i < STEPS_PER_PAGE; i++) {
        stepLedQueue.push([0x09, 0x90, MoveStep1 + i, Black]);
    }
    lastStepLedKey = "";
}

/* ── Play Button LED ─────────────────────────────────────────────── */

function updatePlayButtonLed() {
    const shouldLight = (selectedEntry === songEntries.length);
    if (shouldLight === playButtonLit) return;
    playButtonLit = shouldLight;
    const color = shouldLight ? BrightGreen : Black;
    move_midi_internal_send([0x0B, 0xB0, MovePlay, color]);
}

/* ── Record LED ───────────────────────────────────────────────────── */

function updateRecordLed() {
    const shouldLight = recording;
    if (shouldLight === recordLedLit) return;
    recordLedLit = shouldLight;
    const color = shouldLight ? BrightRed : Black;
    move_midi_internal_send([0x0B, 0xB0, MoveRec, color]);
}

/* ── Recording Helpers ─────────────────────────────────────────────── */

function stopRecording() {
    if (!recording) return;
    host_sampler_stop();
    recording = false;
    recordStopTime = 0;
    recordingSavedUntil = Date.now() + 3000;
    console.log("song-mode: recording stopped");
}

/* ── Pad LED Highlighting ─────────────────────────────────────────── */

function snapshotPadLeds() {
    if (typeof shadow_get_pad_led_snapshot === "function") {
        padLedSnapshot = shadow_get_pad_led_snapshot();
    } else {
        padLedSnapshot = {};
    }
}

function getHighlightKey() {
    if (playbackState === "playing") return "playing";  /* no pad highlights during playback */
    if (selectedEntry >= songEntries.length) return "none";
    const entry = songEntries[selectedEntry];
    return selectedEntry + ":" + entry.pads.join(",");
}

function updatePadHighlights() {
    const key = getHighlightKey();
    if (key !== lastHighlightKey) {
        lastHighlightKey = key;

        /* Determine new highlights */
        const newHighlights = [];
        if (playbackState !== "playing" && selectedEntry < songEntries.length) {
            const entry = songEntries[selectedEntry];
            for (let t = 0; t < NUM_TRACKS; t++) {
                if (entry.pads[t] !== null) {
                    newHighlights.push(padNote(t, entry.pads[t]));
                }
            }
        }

        /* Queue restores for old highlights not in new set */
        const newSet = new Set(newHighlights);
        for (const note of highlightedNotes) {
            if (!newSet.has(note)) {
                const color = padLedSnapshot[note] || 0;
                const existing = restoreRetryQueue.find(r => r.note === note);
                if (existing) { existing.ttl = 3; existing.color = color; }
                else { restoreRetryQueue.push({ note: note, color: color, ttl: 3 }); }
            }
        }

        /* Remove new highlights from restore queue */
        for (const note of newHighlights) {
            restoreRetryQueue = restoreRetryQueue.filter(r => r.note !== note);
        }

        /* Build combined LED queue: restores first, then highlights.
         * All sends go through the queue — nothing sent immediately. */
        ledRetryQueue = [];
        for (const r of restoreRetryQueue) {
            ledRetryQueue.push([0x09, 0x90, r.note, r.color]);
        }
        for (const note of newHighlights) {
            ledRetryQueue.push([0x09, 0x90, note, LED_RED]);
        }
        /* Add a second pass of highlights for reliability */
        for (const note of newHighlights) {
            ledRetryQueue.push([0x09, 0x90, note, LED_RED]);
        }

        highlightedNotes = newHighlights;
    }

    /* Drain up to 2 queued LED packets per tick */
    for (let i = 0; i < 2 && ledRetryQueue.length > 0; i++) {
        move_midi_internal_send(ledRetryQueue.shift());
    }

    /* Tick down restore retries and remove expired ones */
    for (let i = restoreRetryQueue.length - 1; i >= 0; i--) {
        restoreRetryQueue[i].ttl--;
        if (restoreRetryQueue[i].ttl <= 0) restoreRetryQueue.splice(i, 1);
    }
}

function restoreAllPadLeds() {
    for (const note of highlightedNotes) {
        const color = padLedSnapshot[note] || 0;
        move_midi_internal_send([0x09, 0x90, note, color]);
    }
    highlightedNotes = [];
    lastHighlightKey = "";
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

globalThis.init = function() {
    console.log("Song Mode initializing");

    /* Mute Move's audio — transport may be playing when we enter.
     * Unmuted when playback starts (phase 3) or on exit. */
    if (typeof host_mute_move_audio === "function") host_mute_move_audio(1);

    /* Stop transport if it was playing (Play CC toggle).
     * If it was stopped, this briefly starts it (muted), handled by
     * the 2-toggle sequence in startPlayback. */
    move_midi_inject_to_move([0x0B, 0xB0, MovePlay, 127]);
    move_midi_inject_to_move([0x0B, 0xB0, MovePlay, 0]);

    /* Reset state first (before queueing anything) */
    songEntries = [newEntry()];
    selectedEntry = 0;
    playbackState = "stopped";
    injectQueue = [];

    /* Check if user is in Session view — warn briefly if not */
    const uiMode = (typeof shadow_get_move_ui_mode === "function") ? shadow_get_move_ui_mode() : 0;
    console.log("song-mode: move_ui_mode=" + uiMode);
    if (uiMode !== 1) {
        sessionWarning = true;
        announceOverlay("Warning: Switch to Session Mode before using Song Mode");
        console.log("song-mode: NOT in Session view, showing warning screen");
    }

    /* Snapshot pad LED colors before we modify anything */
    snapshotPadLeds();

    /* Verify injection function exists */
    if (typeof move_midi_inject_to_move === "function") {
        console.log("song-mode: move_midi_inject_to_move OK");
    } else {
        console.log("song-mode: WARNING move_midi_inject_to_move NOT available");
    }

    const ok = loadSetData();
    if (ok) {
        console.log("song-mode: loaded set '" + setName + "' tempo=" + tempo);
        /* Log clip grid summary */
        for (let t = 0; t < NUM_TRACKS; t++) {
            let row = "T" + (t+1) + ":";
            for (let c = 0; c < NUM_COLS; c++) {
                row += clipGrid[t][c].exists ? " " + COL_LABELS[c] + "(" + clipGrid[t][c].bars + "b)" : " --";
            }
            console.log("song-mode: " + row);
        }
        /* Find per-track empty slots for silencing before playback */
        for (let t = 0; t < NUM_TRACKS; t++) {
            silencePads[t] = null;
            for (let c = 0; c < NUM_COLS; c++) {
                if (!clipGrid[t][c].exists) {
                    silencePads[t] = c;
                    break;
                }
            }
        }
        console.log("song-mode: silence pads=" + silencePads.map(
            (c, t) => c !== null ? (t+1) + COL_LABELS[c] : (t+1) + "?").join(" "));
    } else {
        console.log("song-mode: no set data, manual entry mode");
    }

    /* Load saved song arrangement for this set (if any) */
    loadSongData();

    announceView("Song Mode" + (setName ? ", " + setName : ""));
};

globalThis.tick = function() {
    tickPlayback();
    drainInjectQueue();

    /* Recording auto-stop: after song ends, wait 1 extra bar then stop both */
    if (recording && recordStopTime > 0 && Date.now() >= recordStopTime) {
        console.log("song-mode: recording auto-stop triggered");
        stopRecording();
        if (playbackState === "playing") stopPlayback();
    }

    /* Step hold detection — open step params after holding for STEP_HOLD_MS */
    if (heldStepNote >= 0 && !heldStepHandled && currentView === "list") {
        if (Date.now() - heldStepTime >= STEP_HOLD_MS) {
            heldStepHandled = true;
            const entryIdx = selectedEntry;
            if (entryIdx < songEntries.length && !isEntryEmpty(songEntries[entryIdx])) {
                pushUndo();
                editingEntry = entryIdx;
                currentView = "step_params";
                stepParamsPeek = true;  /* peek — exits on release */
                const e = songEntries[editingEntry];
                announceView("Step " + (editingEntry + 1) + " Settings, Repeats " + e.repeats + " times");
            }
        }
    }

    /* Persist song arrangement if changed */
    saveSongData();

    updateStepLeds();
    updatePlayButtonLed();
    updateRecordLed();
    updatePadHighlights();
    if (sessionWarning) {
        clear_screen();
        drawMessageOverlay("Warning", [
            "Switch to Session",
            "Mode before using",
            "Song Mode"
        ], true);
        return;
    }
    if (currentView === "step_params") {
        drawStepParamsView();
    } else {
        drawListView();
    }
    if (recordingSavedUntil > 0 && Date.now() < recordingSavedUntil) {
        drawMessageOverlay("Recording", ["Recording saved!"], false);
        if (recordingSavedUntil > 0 && !recordingSavedAnnounced) {
            announceOverlay("Recording saved");
            recordingSavedAnnounced = true;
        }
    } else {
        recordingSavedUntil = 0;
        recordingSavedAnnounced = false;
    }
    if (overlayTimeout > 0 && Date.now() < overlayTimeout) {
        drawMessageOverlay("Song Mode", [overlayMessage], false);
    } else {
        overlayMessage = "";
        overlayTimeout = 0;
    }
};

globalThis.onMidiMessageInternal = function(data) {
    if (isCapacitiveTouchMessage(data)) return;

    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    /* Session warning screen — jog click or back to dismiss */
    if (sessionWarning) {
        if (status === MidiCC && (d1 === MoveMainButton || d1 === MoveBack) && d2 > 0) {
            sessionWarning = false;
            announceView("Song Mode");
        }
        return;
    }

    /* Track shift */
    if (status === MidiCC && d1 === MoveShift) {
        shiftHeld = d2 > 0;
        return;
    }

    /* ── Step Params view controls ── */
    if (currentView === "step_params") {
        /* Jog wheel — cycle repeat values */
        if (status === MidiCC && d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            const entry = songEntries[editingEntry];
            if (entry) {
                let idx = REPEAT_VALUES.indexOf(entry.repeats);
                if (idx < 0) idx = 0;
                idx = Math.max(0, Math.min(REPEAT_VALUES.length - 1, idx + delta));
                entry.repeats = REPEAT_VALUES[idx];
                announceParameter("Repeats", entry.repeats + " times");
            }
            return;
        }
        /* Jog click or Back — return to list */
        if (status === MidiCC && (d1 === MoveMainButton || d1 === MoveBack) && d2 > 0) {
            currentView = "list";
            announce(describeEntry(selectedEntry));
            return;
        }
        /* Step button in step_params view */
        if (status === MidiNoteOn && d1 >= MoveStep1 && d1 <= MoveStep16) {
            if (d2 > 0 && shiftHeld) {
                /* Shift+Step again → exit (sticky mode toggle) */
                currentView = "list";
                announce(describeEntry(selectedEntry));
            } else if (d2 === 0 && stepParamsPeek) {
                /* Release after hold → exit peek mode */
                heldStepNote = -1;
                currentView = "list";
                announce(describeEntry(selectedEntry));
            }
            return;
        }
        return; /* swallow all other input in step_params view */
    }

    /* ── List view controls ── */

    /* Jog wheel — navigate entries + Play/Record Song items at end */
    if (status === MidiCC && d1 === MoveMainKnob) {
        const delta = decodeDelta(d2);
        const maxIdx = songEntries.length + 1; /* extra slots for Play/Record Song */
        selectedEntry = Math.max(0, Math.min(maxIdx, selectedEntry + delta));
        /* Keep step page in sync with selection */
        if (selectedEntry < songEntries.length) {
            stepPage = Math.floor(selectedEntry / STEPS_PER_PAGE);
        }
        announce(describeEntry(selectedEntry));
        return;
    }

    /* Jog click — open step params, play, record, or stop */
    if (status === MidiCC && d1 === MoveMainButton && d2 > 0) {
        if (playbackState === "playing") {
            stopRecording();
            stopPlayback();
            announce("Stopped");
        } else if (selectedEntry === songEntries.length) {
            /* Play Song (from beginning — no step selected) */
            startPlayback(true);
            if (playbackState === "playing") announce("Playing from beginning");
        } else if (selectedEntry === songEntries.length + 1) {
            /* Record Song (from beginning — no step selected) */
            startPlayback(true);
            if (playbackState === "playing") {
                host_ensure_dir(RECORDINGS_DIR);
                const now = new Date();
                const ts = now.getFullYear().toString()
                    + String(now.getMonth() + 1).padStart(2, "0")
                    + String(now.getDate()).padStart(2, "0")
                    + "_" + String(now.getHours()).padStart(2, "0")
                    + String(now.getMinutes()).padStart(2, "0")
                    + String(now.getSeconds()).padStart(2, "0");
                const path = RECORDINGS_DIR + "/song_" + ts + ".wav";
                host_sampler_start(path);
                recording = true;
                recordStartTime = Date.now();
                recordStopTime = 0;
                announce("Recording from beginning");
                console.log("song-mode: recording started -> " + path);
            }
        } else if (selectedEntry < songEntries.length && !isEntryEmpty(songEntries[selectedEntry])) {
            pushUndo();
            editingEntry = selectedEntry;
            currentView = "step_params";
            stepParamsPeek = false;  /* jog click = sticky */
            const e = songEntries[editingEntry];
            announceView("Step " + (editingEntry + 1) + " Settings, Repeats " + e.repeats + " times");
        }
        return;
    }

    /* Delete (X button) — remove selected entry, Shift+Delete clears all */
    if (status === MidiCC && d1 === MoveDelete && d2 > 0) {
        if (shiftHeld) {
            /* Shift+Delete: clear entire song */
            if (countNonEmpty() > 0) {
                pushUndo();
                songEntries = [newEntry()];
                selectedEntry = 0;
                scrollOffset = 0;
                lastStepLedKey = "";
                lastHighlightKey = "";
                announce("Song cleared");
            } else {
                announce("Song is empty");
            }
        } else if (songEntries.length > 1 && selectedEntry < songEntries.length &&
                   !isEntryEmpty(songEntries[selectedEntry])) {
            pushUndo();
            const deletedIdx = selectedEntry + 1;
            songEntries.splice(selectedEntry, 1);
            if (selectedEntry >= songEntries.length) selectedEntry = songEntries.length - 1;
            ensureTrailingEmpty();
            announce("Step " + deletedIdx + " deleted, " + describeEntry(selectedEntry));
        } else {
            announce("Nothing to delete");
        }
        return;
    }

    /* Undo — restore previous song state */
    if (status === MidiCC && d1 === MoveUndo && d2 > 0) {
        if (popUndo()) {
            announce("Undo, " + describeEntry(selectedEntry));
        } else {
            announce("Nothing to undo");
        }
        return;
    }

    /* Copy — duplicate selected entry below (skip if empty) */
    if (status === MidiCC && d1 === MoveCopy && d2 > 0) {
        if (selectedEntry < songEntries.length && !isEntryEmpty(songEntries[selectedEntry])) {
            pushUndo();
            const src = songEntries[selectedEntry];
            const copy = { pads: [...src.pads], repeats: src.repeats };
            songEntries.splice(selectedEntry + 1, 0, copy);
            selectedEntry++;
            ensureTrailingEmpty();
            announce("Copied to step " + (selectedEntry + 1));
        }
        return;
    }

    /* Shift+Up/Down — move selected entry */
    if (status === MidiCC && (d1 === MoveUp || d1 === MoveDown) && d2 > 0 && shiftHeld) {
        if (selectedEntry < songEntries.length && !isEntryEmpty(songEntries[selectedEntry])) {
            const dir = d1 === MoveUp ? -1 : 1;
            const target = selectedEntry + dir;
            if (target >= 0 && target < songEntries.length) {
                pushUndo();
                const tmp = songEntries[selectedEntry];
                songEntries[selectedEntry] = songEntries[target];
                songEntries[target] = tmp;
                selectedEntry = target;
                ensureTrailingEmpty();
                announce("Moved to step " + (selectedEntry + 1));
            }
        }
        return;
    }

    /* Loop button — toggle loop mode */
    if (status === MidiCC && d1 === MoveLoop && d2 > 0) {
        loopEnabled = !loopEnabled;
        announce(loopEnabled ? "Loop on" : "Loop off");
        return;
    }

    /* Back — stop playback or exit */
    if (status === MidiCC && d1 === MoveBack && d2 > 0) {
        stopRecording();
        if (playbackState === "playing") {
            stopPlayback();
        } else {
            restoreAllPadLeds();
            clearStepLeds();
            move_midi_internal_send([0x0B, 0xB0, MovePlay, Black]);
            move_midi_internal_send([0x0B, 0xB0, MoveRec, Black]);
            host_exit_module();
        }
        return;
    }

    /* L/R buttons — page through step pages */
    if (status === MidiCC && d1 === MoveLeft && d2 > 0) {
        if (stepPage > 0) {
            stepPage--;
            selectedEntry = stepPage * STEPS_PER_PAGE;
            lastStepLedKey = ""; /* force refresh */
            announce("Page " + (stepPage + 1) + ", " + describeEntry(selectedEntry));
        }
        return;
    }
    if (status === MidiCC && d1 === MoveRight && d2 > 0) {
        stepPage++;
        selectedEntry = stepPage * STEPS_PER_PAGE;
        ensureEntryExists(selectedEntry);
        ensureTrailingEmpty();
        lastStepLedKey = ""; /* force refresh */
        announce("Page " + (stepPage + 1) + ", " + describeEntry(selectedEntry));
        return;
    }

    /* Step buttons — tap to select, hold or Shift+tap to open step params */
    if (status === MidiNoteOn && d1 >= MoveStep1 && d1 <= MoveStep16 && d2 > 0) {
        const stepIdx = d1 - MoveStep1;
        const entryIdx = stepPage * STEPS_PER_PAGE + stepIdx;
        if (shiftHeld && entryIdx < songEntries.length && !isEntryEmpty(songEntries[entryIdx])) {
            /* Shift+Step: open step params (sticky — stays until Shift+Step again) */
            pushUndo();
            selectedEntry = entryIdx;
            editingEntry = entryIdx;
            currentView = "step_params";
            stepParamsPeek = false;
            heldStepNote = -1;
            const e = songEntries[editingEntry];
            announceView("Step " + (editingEntry + 1) + " Settings, Repeats " + e.repeats + " times");
        } else {
            /* Track hold — select on release if short press */
            heldStepNote = d1;
            heldStepTime = Date.now();
            heldStepHandled = false;
            selectStep(stepIdx);
            announce(describeEntry(selectedEntry));
        }
        return;
    }
    /* Step button release */
    if (status === MidiNoteOn && d1 >= MoveStep1 && d1 <= MoveStep16 && d2 === 0) {
        heldStepNote = -1;
        return;
    }

    /* Play button — Play from current step, Shift+Play from beginning */
    if (status === MidiCC && d1 === MovePlay && d2 > 0) {
        if (playbackState === "playing") {
            stopRecording();
            stopPlayback();
            announce("Stopped");
        } else {
            startPlayback(shiftHeld);
            if (playbackState === "playing") {
                announce("Playing from step " + (currentEntryIndex + 1));
            }
        }
        return;
    }

    /* Record button — Rec from current step, Shift+Rec from beginning */
    if (status === MidiCC && d1 === MoveRec && d2 > 0) {
        if (recording) {
            /* Stop recording */
            stopRecording();
            announce("Recording stopped");
        } else {
            /* Start recording — begin playback if not already playing */
            if (playbackState !== "playing") {
                startPlayback(shiftHeld);
            }
            if (playbackState === "playing") {
                host_ensure_dir(RECORDINGS_DIR);
                const now = new Date();
                const ts = now.getFullYear().toString()
                    + String(now.getMonth() + 1).padStart(2, "0")
                    + String(now.getDate()).padStart(2, "0")
                    + "_" + String(now.getHours()).padStart(2, "0")
                    + String(now.getMinutes()).padStart(2, "0")
                    + String(now.getSeconds()).padStart(2, "0");
                const path = RECORDINGS_DIR + "/song_" + ts + ".wav";
                host_sampler_start(path);
                recording = true;
                recordStartTime = Date.now();
                recordStopTime = 0;
                announce("Recording from step " + (currentEntryIndex + 1));
                console.log("song-mode: recording started -> " + path);
            }
        }
        return;
    }

    /* Pads — set assignment on selected entry (all 4 tracks always populated) */
    if (status === MidiNoteOn && d2 > 0) {
        const grid = noteToGrid(d1);
        if (grid && selectedEntry < songEntries.length) {
            pushUndo();
            const entry = songEntries[selectedEntry];
            /* On first pad press of an empty entry, pre-fill ALL tracks from previous */
            if (isEntryEmpty(entry)) {
                for (let i = selectedEntry - 1; i >= 0; i--) {
                    if (!isEntryEmpty(songEntries[i])) {
                        entry.pads = [...songEntries[i].pads];
                        break;
                    }
                }
                /* If no previous entry, default all tracks to column A */
                if (isEntryEmpty(entry)) {
                    entry.pads = [0, 0, 0, 0];
                }
            }
            /* Set the pressed track (no deselect — can't have null) */
            entry.pads[grid.track] = grid.col;
            ensureTrailingEmpty();
            announce("Track " + (grid.track + 1) + " set to " + COL_LABELS[grid.col]);
        }
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {};
