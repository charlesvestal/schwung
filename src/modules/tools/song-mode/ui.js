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
    MovePlay, MoveDelete, MoveCopy
} from '/data/UserData/move-anything/shared/constants.mjs';

import {
    isCapacitiveTouchMessage, decodeDelta
} from '/data/UserData/move-anything/shared/input_filter.mjs';

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

/* Per-track empty slot column index (for silencing tracks before playback) */
let silencePads = [null, null, null, null];

/* Song arrangement — always ends with one empty row */
let songEntries = [];
let selectedEntry = 0;
let scrollOffset = 0;

/* Playback */
let playbackState = "stopped";
let currentEntryIndex = 0;
let playStartTime = 0;
let nextEntryQueued = false;

/* MIDI inject queue — drain packets per tick */
let injectQueue = [];

/* Deferred note-offs: [note, ticksRemaining] */
let pendingNoteOffs = [];

/* Shift tracking */
let shiftHeld = false;

/* Pad LED highlighting */
const LED_RED = 1;
let padLedSnapshot = {};    /* note -> original color from Move */
let highlightedNotes = [];  /* currently red-highlighted pad notes */
let lastHighlightKey = "";  /* change detection */

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
            const regionEnd = slot.clip.region ? slot.clip.region.end : 0;
            clipGrid[t][c] = { exists: true, bars: regionEnd > 0 ? regionEnd / 4 : 0 };
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

function isEntryEmpty(entry) {
    return entry.pads.every(p => p === null);
}

function isEntryComplete(entry) {
    return entry.pads.every(p => p !== null);
}

function newEntry() {
    return { pads: [null, null, null, null] };
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

const MAX_VISIBLE = 5;

function drawListView() {
    clear_screen();

    /* Header */
    print(2, 2, "Song Mode", 1);
    const nameStr = setName || "No Set";
    const nameW = nameStr.length * 6;
    print(Math.max(128 - nameW - 2, 60), 2, nameStr, 1);
    fill_rect(0, 12, 128, 1, 1);

    /* Total items: songEntries + 1 "Play Song" item at end */
    const totalItems = songEntries.length + 1;

    /* Ensure selected is in bounds */
    if (selectedEntry >= totalItems) selectedEntry = totalItems - 1;
    if (selectedEntry < 0) selectedEntry = 0;

    /* Scroll */
    if (selectedEntry < scrollOffset) scrollOffset = selectedEntry;
    if (selectedEntry >= scrollOffset + MAX_VISIBLE) scrollOffset = selectedEntry - MAX_VISIBLE + 1;

    for (let i = 0; i < MAX_VISIBLE; i++) {
        const idx = scrollOffset + i;
        if (idx >= totalItems) break;

        const y = 15 + i * 9;
        const isSelected = idx === selectedEntry;

        if (isSelected) {
            fill_rect(0, y - 1, 128, 9, 1);
        }
        const color = isSelected ? 0 : 1;

        /* "Play Song" item at end */
        if (idx === songEntries.length) {
            const playLabel = playbackState === "playing" ? ">> Stop Song" : ">> Play Song";
            print(2, y, playLabel, color);
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
            const bars = entryBars(entry);
            const barStr = bars + "b";
            print(128 - barStr.length * 6 - 2, y, barStr, color);
        }
    }

    /* Scroll indicators */
    if (scrollOffset > 0) print(122, 13, "^", 1);
    if (scrollOffset + MAX_VISIBLE < totalItems) print(122, 52, "v", 1);

    /* Footer */
    fill_rect(0, 55, 128, 1, 1);
    if (playbackState === "playing") {
        const entry = songEntries[currentEntryIndex];
        const bars = entryBars(entry);
        const elapsed = (Date.now() - playStartTime) / barDurationMs;
        const currentBar = Math.min(Math.floor(elapsed) + 1, bars);
        print(2, 57, "Playing " + (currentEntryIndex + 1) + "/" + countNonEmpty() + " bar " + currentBar + "/" + bars, 1);
    } else {
        print(2, 57, "Pad:set X:del Click:play", 1);
    }
}

function countNonEmpty() {
    let n = 0;
    for (const e of songEntries) if (!isEntryEmpty(e)) n++;
    return n;
}

/* ── Playback Engine ───────────────────────────────────────────────── */

function startPlayback() {
    /* Find first non-empty, complete entry */
    let startIdx = -1;
    for (let i = 0; i < songEntries.length; i++) {
        if (!isEntryEmpty(songEntries[i]) && isEntryComplete(songEntries[i])) {
            startIdx = i;
            break;
        }
    }
    if (startIdx < 0) { console.log("song-mode: no complete entries to play"); return; }

    console.log("song-mode: startPlayback from entry " + startIdx);
    currentEntryIndex = startIdx;

    /* Stop any running transport first via MIDI Stop (unconditional).
     * Then select empty clips on tracks that have them (clears old sound).
     * Then select the actual entry clips (auto-starts transport fresh). */
    injectQueue.push([0x0F, 0xFC, 0x00, 0x00]); /* MIDI Stop */
    for (let t = 0; t < NUM_TRACKS; t++) {
        if (silencePads[t] !== null) {
            const note = padNote(t, silencePads[t]);
            queueInjectNote(note, 127);
            pendingNoteOffs.push({ note: note, ticks: 15 });
        }
    }
    triggerEntry(startIdx);

    playStartTime = Date.now();
    nextEntryQueued = false;
    playbackState = "playing";
}

function stopPlayback() {
    playbackState = "stopped";
    injectQueue = [];
    pendingNoteOffs = [];
    /* Select silent/empty clips to stop sound, then stop transport */
    for (let t = 0; t < NUM_TRACKS; t++) {
        if (silencePads[t] !== null) {
            queueInjectNote(padNote(t, silencePads[t]), 127);
        }
    }
    queueInjectCC(MovePlay, 127);
    queueInjectCC(MovePlay, 0);
    console.log("song-mode: stopPlayback");
}

function nextCompleteEntry(afterIdx) {
    for (let i = afterIdx + 1; i < songEntries.length; i++) {
        if (!isEntryEmpty(songEntries[i]) && isEntryComplete(songEntries[i])) return i;
    }
    return -1;
}

function tickPlayback() {
    if (playbackState !== "playing") return;
    if (currentEntryIndex >= songEntries.length) { stopPlayback(); return; }

    const entry = songEntries[currentEntryIndex];
    const bars = entryBars(entry);
    const elapsed = (Date.now() - playStartTime) / barDurationMs;

    /* Pre-trigger next entry 1 beat (0.25 bars) before end.
     * Move quantizes clip launches to bar boundaries, so selecting
     * the next clips just before the boundary makes them start on time. */
    const nextIdx = nextCompleteEntry(currentEntryIndex);
    if (!nextEntryQueued && elapsed >= bars - 0.25 && nextIdx >= 0) {
        console.log("song-mode: pre-trigger entry " + nextIdx + " at " + elapsed.toFixed(2) + "/" + bars + " bars");
        triggerEntry(nextIdx);
        nextEntryQueued = true;
    }

    /* Advance when current entry's duration has elapsed */
    if (elapsed >= bars) {
        if (nextIdx < 0) {
            console.log("song-mode: song ended");
            stopPlayback();
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

function drainInjectQueue() {
    /* Drain exactly 1 packet per tick. Move's MIDI_IN buffer only has
     * ~1 empty slot per ioctl cycle (~2.9ms). Sending more than 1
     * causes packets to be overwritten before Move reads them.
     * JS tick runs at ~16ms (~5-6 ioctl cycles), so 1/tick is safe. */
    if (injectQueue.length > 0) {
        const pkt = injectQueue.shift();
        console.log("song-mode: inject [" + pkt.join(",") + "] remaining=" + injectQueue.length);
        move_midi_inject_to_move(pkt);
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
}

/* Immediately inject a single MIDI packet (for pad passthrough) */
function injectNow(cin, status, d1, d2) {
    move_midi_inject_to_move([cin, status, d1, d2]);
}

/* ── Pad LED Highlighting ─────────────────────────────────────────── */

function snapshotPadLeds() {
    if (typeof shadow_get_pad_led_snapshot === "function") {
        padLedSnapshot = shadow_get_pad_led_snapshot();
        console.log("song-mode: pad LED snapshot captured");
    }
}

function getHighlightKey() {
    if (selectedEntry >= songEntries.length) return "play";
    const entry = songEntries[selectedEntry];
    return selectedEntry + ":" + entry.pads.join(",");
}

function updatePadHighlights() {
    const key = getHighlightKey();
    if (key === lastHighlightKey) return;
    lastHighlightKey = key;

    /* Determine new highlight set */
    let newNotes = [];
    if (selectedEntry < songEntries.length) {
        const entry = songEntries[selectedEntry];
        for (let t = 0; t < NUM_TRACKS; t++) {
            if (entry.pads[t] !== null) {
                newNotes.push(padNote(t, entry.pads[t]));
            }
        }
    }

    /* Restore old highlights to original colors */
    for (const note of highlightedNotes) {
        if (newNotes.indexOf(note) < 0) {
            const orig = padLedSnapshot[note];
            const color = (orig !== undefined && orig >= 0) ? orig : 0;
            move_midi_internal_send([0x09, 0x90, note, color]);
        }
    }

    /* Set new highlights to red */
    for (const note of newNotes) {
        move_midi_internal_send([0x09, 0x90, note, LED_RED]);
    }

    highlightedNotes = newNotes;
}

function restoreAllPadLeds() {
    for (const note of highlightedNotes) {
        const orig = padLedSnapshot[note];
        const color = (orig !== undefined && orig >= 0) ? orig : 0;
        move_midi_internal_send([0x09, 0x90, note, color]);
    }
    highlightedNotes = [];
    lastHighlightKey = "";
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

globalThis.init = function() {
    console.log("Song Mode initializing");

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

    songEntries = [newEntry()];
    selectedEntry = 0;
    playbackState = "stopped";
    injectQueue = [];
};

globalThis.tick = function() {
    tickPlayback();
    drainInjectQueue();
    updatePadHighlights();
    drawListView();
};

globalThis.onMidiMessageInternal = function(data) {
    if (isCapacitiveTouchMessage(data)) return;

    const status = data[0] & 0xF0;
    const d1 = data[1];
    const d2 = data[2];

    /* Track shift */
    if (status === MidiCC && d1 === MoveShift) {
        shiftHeld = d2 > 0;
        return;
    }

    /* Jog wheel — navigate entries + Play Song item at end */
    if (status === MidiCC && d1 === MoveMainKnob) {
        const delta = decodeDelta(d2);
        const maxIdx = songEntries.length; /* extra slot for "Play Song" */
        selectedEntry = Math.max(0, Math.min(maxIdx, selectedEntry + delta));
        return;
    }

    /* Jog click — Play Song if on the play item, or stop if playing */
    if (status === MidiCC && d1 === MoveMainButton && d2 > 0) {
        if (playbackState === "playing") {
            stopPlayback();
        } else if (selectedEntry === songEntries.length) {
            startPlayback();
        }
        return;
    }

    /* Delete (X button) — remove selected entry if not the trailing empty */
    if (status === MidiCC && d1 === MoveDelete && d2 > 0) {
        if (songEntries.length > 1 && selectedEntry < songEntries.length) {
            songEntries.splice(selectedEntry, 1);
            if (selectedEntry >= songEntries.length) selectedEntry = songEntries.length - 1;
            ensureTrailingEmpty();
        }
        return;
    }

    /* Copy — duplicate selected entry below */
    if (status === MidiCC && d1 === MoveCopy && d2 > 0) {
        if (selectedEntry < songEntries.length) {
            const src = songEntries[selectedEntry];
            const copy = { pads: [...src.pads] };
            songEntries.splice(selectedEntry + 1, 0, copy);
            selectedEntry++;
            ensureTrailingEmpty();
        }
        return;
    }

    /* Back — stop playback or exit */
    if (status === MidiCC && d1 === MoveBack && d2 > 0) {
        if (playbackState === "playing") {
            stopPlayback();
        } else {
            restoreAllPadLeds();
            host_exit_module();
        }
        return;
    }

    /* Play CC — ignored. Use "Play Song" menu item instead
     * to avoid confusion with Move's transport. */

    /* Pads — toggle assignment on selected entry (blocked from Move during editing) */
    if (status === MidiNoteOn && d2 > 0) {
        const grid = noteToGrid(d1);
        if (grid && selectedEntry < songEntries.length) {
            const entry = songEntries[selectedEntry];
            /* On first pad press of an empty entry, pre-fill from previous entry */
            if (isEntryEmpty(entry)) {
                for (let i = selectedEntry - 1; i >= 0; i--) {
                    if (!isEntryEmpty(songEntries[i])) {
                        entry.pads = [...songEntries[i].pads];
                        break;
                    }
                }
            }
            if (entry.pads[grid.track] === grid.col) {
                entry.pads[grid.track] = null;
            } else {
                entry.pads[grid.track] = grid.col;
            }
            ensureTrailingEmpty();
        }
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {};
