/*
 * SEQOMD UI
 * 8 tracks with track selection via track buttons + shift
 * Move is master - sends MIDI clock to external devices
 */

import * as std from 'std';
import * as os from 'os';

import {
    Black, White, LightGrey, Navy, BrightGreen, Cyan, BrightRed,
    OrangeRed, VividYellow, RoyalBlue, Purple,
    MidiNoteOn, MidiNoteOff, MidiCC,
    MovePlay, MoveLoop, MoveSteps, MovePads, MoveTracks, MoveShift, MoveMenu, MoveRec, MoveRecord,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveKnob1Touch, MoveKnob2Touch, MoveKnob7Touch, MoveKnob8Touch,
    MoveStep1UI, MoveMainKnob, MoveMasterTouch
} from "../../shared/constants.mjs";

import {
    isNoiseMessage, isCapacitiveTouchMessage,
    setLED, setButtonLED, clearAllLEDs
} from "../../shared/input_filter.mjs";

/* Persistent storage path */
const DATA_DIR = '/data/UserData/move-anything-data/sequencer';
const SETS_FILE = DATA_DIR + '/sets.json';

/* Knob LED CCs (same as knob input CCs) */
const MoveKnobLEDs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];

/* ============ Constants ============ */

const NUM_TRACKS = 8;
const NUM_STEPS = 16;

/* Track colors */
const TRACK_COLORS = [
    BrightRed,    // Track 1 - Kick
    OrangeRed,    // Track 2 - Snare
    VividYellow,  // Track 3 - Perc
    BrightGreen,  // Track 4 - Sample
    Cyan,         // Track 5 - Bass
    RoyalBlue,    // Track 6 - Lead
    Purple,       // Track 7 - Arp
    White         // Track 8 - Chord
];

const TRACK_NAMES = [
    "Kick", "Snare", "Perc", "Sample",
    "Bass", "Lead", "Arp", "Chord"
];

/* Dim versions of track colors for non-selected tracks with content */
const TRACK_COLORS_DIM = [
    65,           // Deep Red (dim red)
    67,           // Brick (dim orange)
    73,           // Dull Yellow
    79,           // Dull Green (using index 79)
    87,           // Dark Teal (dim cyan)
    17,           // Navy (dim blue)
    107,          // Dark Purple
    118           // Light Grey (dim white)
];


/* ============ State ============ */

let playing = false;
let currentTrack = 0;           // 0-7
let shiftHeld = false;
let masterMode = false;         // True when in master track view
let heldStep = -1;              // Currently held step for editing (most recent)
let stepPressTimes = {};        // Per-step press timestamps for quick tap detection
let stepPadPressed = {};        // Per-step flag if pad was pressed while held
let sendClock = 1;
let currentPlayStep = -1;       // Current playhead position (0-15)
let lastPlayedNote = -1;        // Last note that was played (for pad display)
let lastSelectedNote = -1;      // Last pad pressed (for quick step entry)
let recording = false;          // Recording mode
let heldPads = new Set();       // Currently held pads (for recording)
let lastRecordedStep = -1;      // Last step we recorded to (avoid double-recording)
let loopEditMode = false;       // Loop button held - editing loop points
let loopEditFirst = -1;         // First step pressed while in loop edit
let patternMode = false;        // Pattern view mode (Menu without shift)
let patternViewOffset = 0;      // Which row of patterns to show (0=1-4, 4=5-8, etc.)
let mainKnobTouched = false;    // Is the main jog wheel being touched
let sparkMode = false;          // Spark edit mode (shift + step to enter, shift to exit)
let sparkSelectedSteps = new Set();  // Steps selected for spark editing

/* Global BPM (mirrors DSP value) */
let bpm = 120;

/* Set view mode - 32 sets, each containing tracks + bpm */
const NUM_SETS = 32;
let setView = true;             // Start in set view mode
let currentSet = 0;             // Currently loaded set (0-31)
let sets = [];                  // Array of 32 sets: null or {tracks, bpm}


/* CC output values for knobs */
const MASTER_CC_CHANNEL = 15;   // Channel 16 (0-indexed) for pattern mode CCs
let patternCCValues = [64, 64, 64, 64, 64, 64, 64, 64];  // 8 knobs in pattern mode
let trackCCValues = [];         // 2 CCs per track [track0_cc1, track0_cc2, track1_cc1, ...]
for (let t = 0; t < NUM_TRACKS; t++) {
    trackCCValues.push(64, 64);  // Initialize to center (64)
}

const HOLD_THRESHOLD_MS = 300;  // Time threshold for hold vs tap

/* Knob touch tracking for tap-to-clear/toggle
 * Indices: 0=knob1, 1=knob2, 6=knob7, 7=knob8
 */
let knobTouchTime = {};   // Timestamp when knob was touched (-1 = not touched)
let knobTurned = {};      // Whether knob was turned while touched

/* Speed options: index -> {name, multiplier} */
const SPEED_OPTIONS = [
    { name: "1/4x", mult: 0.25 },
    { name: "1/3x", mult: 1/3 },
    { name: "1/2x", mult: 0.5 },
    { name: "2/3x", mult: 2/3 },
    { name: "1x",   mult: 1.0 },
    { name: "3/2x", mult: 1.5 },
    { name: "2x",   mult: 2.0 },
    { name: "3x",   mult: 3.0 },
    { name: "4x",   mult: 4.0 }
];
const DEFAULT_SPEED_INDEX = 4;  // 1x

/* Per-track step data (mirrors DSP state) */
const NUM_PATTERNS = 8;
let tracks = [];

/* Ratchet options */
const RATCHET_VALUES = [1, 2, 3, 4, 6, 8];

/* Condition options (index 0 = none/use probability)
 * n = loop cycle length, m = which iteration to play on
 * Example: 2:3 means "play on the 2nd of every 3 loops"
 */
const CONDITIONS = [
    { name: "---", n: 0, m: 0, not: false },   // No condition (use probability)
    // Every 2 loops
    { name: "1:2", n: 2, m: 1, not: false },
    { name: "2:2", n: 2, m: 2, not: false },
    // Every 3 loops
    { name: "1:3", n: 3, m: 1, not: false },
    { name: "2:3", n: 3, m: 2, not: false },
    { name: "3:3", n: 3, m: 3, not: false },
    // Every 4 loops
    { name: "1:4", n: 4, m: 1, not: false },
    { name: "2:4", n: 4, m: 2, not: false },
    { name: "3:4", n: 4, m: 3, not: false },
    { name: "4:4", n: 4, m: 4, not: false },
    // Every 5 loops
    { name: "1:5", n: 5, m: 1, not: false },
    { name: "2:5", n: 5, m: 2, not: false },
    { name: "3:5", n: 5, m: 3, not: false },
    { name: "4:5", n: 5, m: 4, not: false },
    { name: "5:5", n: 5, m: 5, not: false },
    // Every 6 loops
    { name: "1:6", n: 6, m: 1, not: false },
    { name: "2:6", n: 6, m: 2, not: false },
    { name: "3:6", n: 6, m: 3, not: false },
    { name: "4:6", n: 6, m: 4, not: false },
    { name: "5:6", n: 6, m: 5, not: false },
    { name: "6:6", n: 6, m: 6, not: false },
    // Every 8 loops
    { name: "1:8", n: 8, m: 1, not: false },
    { name: "2:8", n: 8, m: 2, not: false },
    { name: "3:8", n: 8, m: 3, not: false },
    { name: "4:8", n: 8, m: 4, not: false },
    { name: "5:8", n: 8, m: 5, not: false },
    { name: "6:8", n: 8, m: 6, not: false },
    { name: "7:8", n: 8, m: 7, not: false },
    { name: "8:8", n: 8, m: 8, not: false },
    // Inverted conditions (NOT - play on all EXCEPT this iteration)
    { name: "!1:2", n: 2, m: 1, not: true },
    { name: "!2:2", n: 2, m: 2, not: true },
    { name: "!1:3", n: 3, m: 1, not: true },
    { name: "!2:3", n: 3, m: 2, not: true },
    { name: "!3:3", n: 3, m: 3, not: true },
    { name: "!1:4", n: 4, m: 1, not: true },
    { name: "!2:4", n: 4, m: 2, not: true },
    { name: "!3:4", n: 4, m: 3, not: true },
    { name: "!4:4", n: 4, m: 4, not: true },
    { name: "!1:5", n: 5, m: 1, not: true },
    { name: "!2:5", n: 5, m: 2, not: true },
    { name: "!3:5", n: 5, m: 3, not: true },
    { name: "!4:5", n: 5, m: 4, not: true },
    { name: "!5:5", n: 5, m: 5, not: true },
    { name: "!1:6", n: 6, m: 1, not: true },
    { name: "!2:6", n: 6, m: 2, not: true },
    { name: "!3:6", n: 6, m: 3, not: true },
    { name: "!4:6", n: 6, m: 4, not: true },
    { name: "!5:6", n: 6, m: 5, not: true },
    { name: "!6:6", n: 6, m: 6, not: true },
    { name: "!1:8", n: 8, m: 1, not: true },
    { name: "!2:8", n: 8, m: 2, not: true },
    { name: "!3:8", n: 8, m: 3, not: true },
    { name: "!4:8", n: 8, m: 4, not: true },
    { name: "!5:8", n: 8, m: 5, not: true },
    { name: "!6:8", n: 8, m: 6, not: true },
    { name: "!7:8", n: 8, m: 7, not: true },
    { name: "!8:8", n: 8, m: 8, not: true }
];

/* Create a new empty step */
function createEmptyStep() {
    return {
        notes: [],      // Array of MIDI notes
        cc1: -1,        // CC value for knob 1 (-1 = not set)
        cc2: -1,        // CC value for knob 2 (-1 = not set)
        probability: 100,  // 1-100%
        condition: 0,      // Index into CONDITIONS (0 = none) - TRIGGER SPARK
        ratchet: 0,        // Index into RATCHET_VALUES (0 = 1x, no ratchet)
        length: 1,         // Note length in steps (1-16)
        paramSpark: 0,     // Index into CONDITIONS - when CC locks apply
        compSpark: 0,      // Index into CONDITIONS - when ratchet/jump apply
        jump: -1           // Jump target step (-1 = no jump, 0-15 = step to jump to)
    };
}

/* Create empty tracks structure */
function createEmptyTracks() {
    const newTracks = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        newTracks.push({
            patterns: [],
            currentPattern: 0,
            muted: false,
            channel: t,
            speedIndex: DEFAULT_SPEED_INDEX
        });
        for (let p = 0; p < NUM_PATTERNS; p++) {
            newTracks[t].patterns.push({
                steps: [],
                loopStart: 0,
                loopEnd: NUM_STEPS - 1
            });
            for (let s = 0; s < NUM_STEPS; s++) {
                newTracks[t].patterns[p].steps.push(createEmptyStep());
            }
        }
    }
    return newTracks;
}

/* Deep clone tracks for saving to a set */
function deepCloneTracks(srcTracks) {
    const clone = [];
    for (let t = 0; t < NUM_TRACKS; t++) {
        const srcTrack = srcTracks[t];
        clone.push({
            patterns: [],
            currentPattern: srcTrack.currentPattern,
            muted: srcTrack.muted,
            channel: srcTrack.channel,
            speedIndex: srcTrack.speedIndex
        });
        for (let p = 0; p < NUM_PATTERNS; p++) {
            const srcPattern = srcTrack.patterns[p];
            clone[t].patterns.push({
                steps: [],
                loopStart: srcPattern.loopStart,
                loopEnd: srcPattern.loopEnd
            });
            for (let s = 0; s < NUM_STEPS; s++) {
                const srcStep = srcPattern.steps[s];
                clone[t].patterns[p].steps.push({
                    notes: [...srcStep.notes],
                    cc1: srcStep.cc1,
                    cc2: srcStep.cc2,
                    probability: srcStep.probability,
                    condition: srcStep.condition,
                    ratchet: srcStep.ratchet,
                    length: srcStep.length || 1,
                    paramSpark: srcStep.paramSpark,
                    compSpark: srcStep.compSpark,
                    jump: srcStep.jump
                });
            }
        }
    }
    return clone;
}

/* Check if a set has any content */
function setHasContent(setIdx) {
    if (!sets[setIdx]) return false;
    const setTracks = sets[setIdx].tracks || sets[setIdx];  /* Handle both old and new format */
    for (const track of setTracks) {
        for (const pattern of track.patterns) {
            for (const step of pattern.steps) {
                if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

/* Save current tracks to current set (in memory) */
function saveCurrentSet() {
    sets[currentSet] = {
        tracks: deepCloneTracks(tracks),
        bpm: bpm
    };
}

/* Ensure data directory exists */
function ensureDataDir() {
    try {
        os.mkdir(DATA_DIR, 0o755);
    } catch (e) {
        /* Directory may already exist, that's fine */
    }
    /* Also ensure parent exists */
    try {
        os.mkdir('/data/UserData/move-anything-data', 0o755);
    } catch (e) {
        /* Directory may already exist */
    }
}

/* Save all sets to disk */
function saveAllSetsToDisk() {
    ensureDataDir();
    try {
        const f = std.open(SETS_FILE, 'w');
        if (f) {
            f.puts(JSON.stringify(sets));
            f.close();
            console.log('Sets saved to disk');
            return true;
        }
    } catch (e) {
        console.log('Failed to save sets: ' + e);
    }
    return false;
}

/* Load all sets from disk */
function loadAllSetsFromDisk() {
    try {
        const content = std.loadFile(SETS_FILE);
        if (content) {
            const loaded = JSON.parse(content);
            if (Array.isArray(loaded) && loaded.length === NUM_SETS) {
                sets = loaded;
                console.log('Sets loaded from disk');
                return true;
            }
        }
    } catch (e) {
        console.log('No saved sets found or failed to load: ' + e);
    }
    return false;
}

/* Load a set into current tracks and sync to DSP */
function loadSetToTracks(setIdx) {
    if (!sets[setIdx]) {
        sets[setIdx] = {
            tracks: createEmptyTracks(),
            bpm: 120
        };
    }

    /* Handle both old format (array) and new format ({tracks, bpm}) */
    const setData = sets[setIdx];
    const setTracks = setData.tracks || setData;
    const setBpm = setData.bpm || 120;

    tracks = deepCloneTracks(setTracks);
    bpm = setBpm;
    currentSet = setIdx;

    /* Sync BPM to DSP */
    host_module_set_param("bpm", String(bpm));

    /* Sync all track data to DSP
     * DSP only supports step operations on current pattern,
     * so we iterate through each pattern, set it current, sync, then restore
     */
    for (let t = 0; t < NUM_TRACKS; t++) {
        const track = tracks[t];
        host_module_set_param(`track_${t}_channel`, String(track.channel));
        host_module_set_param(`track_${t}_muted`, track.muted ? "1" : "0");
        host_module_set_param(`track_${t}_speed`, String(SPEED_OPTIONS[track.speedIndex].mult));

        /* Sync all patterns by temporarily setting each as current */
        for (let p = 0; p < NUM_PATTERNS; p++) {
            /* Set this pattern as current so step commands affect it */
            host_module_set_param(`track_${t}_pattern`, String(p));

            const pattern = track.patterns[p];
            host_module_set_param(`track_${t}_loop_start`, String(pattern.loopStart));
            host_module_set_param(`track_${t}_loop_end`, String(pattern.loopEnd));

            for (let s = 0; s < NUM_STEPS; s++) {
                const step = pattern.steps[s];

                /* Clear step first, then add notes if any */
                host_module_set_param(`track_${t}_step_${s}_clear`, "1");

                for (const note of step.notes) {
                    host_module_set_param(`track_${t}_step_${s}_add_note`, String(note));
                }

                if (step.cc1 >= 0) {
                    host_module_set_param(`track_${t}_step_${s}_cc1`, String(step.cc1));
                }
                if (step.cc2 >= 0) {
                    host_module_set_param(`track_${t}_step_${s}_cc2`, String(step.cc2));
                }
                host_module_set_param(`track_${t}_step_${s}_probability`, String(step.probability));
                host_module_set_param(`track_${t}_step_${s}_condition_n`, String(CONDITIONS[step.condition].n));
                host_module_set_param(`track_${t}_step_${s}_condition_m`, String(CONDITIONS[step.condition].m));
                host_module_set_param(`track_${t}_step_${s}_condition_not`, CONDITIONS[step.condition].not ? "1" : "0");
                host_module_set_param(`track_${t}_step_${s}_ratchet`, String(step.ratchet > 0 ? RATCHET_VALUES[step.ratchet] : 1));
                host_module_set_param(`track_${t}_step_${s}_length`, String(step.length || 1));
                host_module_set_param(`track_${t}_step_${s}_param_spark_n`, String(CONDITIONS[step.paramSpark].n));
                host_module_set_param(`track_${t}_step_${s}_param_spark_m`, String(CONDITIONS[step.paramSpark].m));
                host_module_set_param(`track_${t}_step_${s}_param_spark_not`, CONDITIONS[step.paramSpark].not ? "1" : "0");
                host_module_set_param(`track_${t}_step_${s}_comp_spark_n`, String(CONDITIONS[step.compSpark].n));
                host_module_set_param(`track_${t}_step_${s}_comp_spark_m`, String(CONDITIONS[step.compSpark].m));
                host_module_set_param(`track_${t}_step_${s}_comp_spark_not`, CONDITIONS[step.compSpark].not ? "1" : "0");
                host_module_set_param(`track_${t}_step_${s}_jump`, String(step.jump));
            }
        }

        /* Restore the actual current pattern */
        host_module_set_param(`track_${t}_pattern`, String(track.currentPattern));
    }
}

for (let t = 0; t < NUM_TRACKS; t++) {
    tracks.push({
        patterns: [],  // Array of patterns, each pattern has steps
        currentPattern: 0,
        muted: false,
        channel: t,
        speedIndex: DEFAULT_SPEED_INDEX  // Index into SPEED_OPTIONS
    });
    /* Initialize 8 patterns per track */
    for (let p = 0; p < NUM_PATTERNS; p++) {
        tracks[t].patterns.push({
            steps: [],
            loopStart: 0,
            loopEnd: NUM_STEPS - 1
        });
        for (let s = 0; s < NUM_STEPS; s++) {
            tracks[t].patterns[p].steps.push(createEmptyStep());
        }
    }
}

/* Helper to get current pattern for a track */
function getCurrentPattern(trackIdx) {
    return tracks[trackIdx].patterns[tracks[trackIdx].currentPattern];
}

/* Master track data */
const TRACK_TYPE_DRUM = 0;
const TRACK_TYPE_NOTE = 1;
const TRACK_TYPE_ARP = 2;
const TRACK_TYPE_CHORD = 3;
const TRACK_TYPE_NAMES = ["Drum", "Note", "Arp", "Chord"];
const TRACK_TYPE_COLORS = [BrightRed, BrightGreen, Cyan, VividYellow];

let masterData = {
    followChord: new Array(NUM_TRACKS).fill(false),  // Which tracks follow master chord
    trackType: [TRACK_TYPE_DRUM, TRACK_TYPE_DRUM, TRACK_TYPE_DRUM, TRACK_TYPE_DRUM,
                TRACK_TYPE_NOTE, TRACK_TYPE_NOTE, TRACK_TYPE_ARP, TRACK_TYPE_CHORD],  // Default types
    rootNote: 0,       // 0-11 (C to B)
    scale: 0           // 0=Major, 1=Minor, etc.
};

/* Display state */
let line1 = "SEQOMD";
let line2 = "Track 1";
let line3 = "Press Play";
let line4 = "";

/* ============ Display ============ */

function drawUI() {
    clear_screen();
    print(2, 2, line1, 1);
    print(2, 18, line2, 1);
    print(2, 34, line3, 1);
    print(2, 50, line4, 1);
}

function displayMessage(l1, l2, l3, l4) {
    if (l1 !== undefined) line1 = l1;
    if (l2 !== undefined) line2 = l2;
    if (l3 !== undefined) line3 = l3;
    if (l4 !== undefined) line4 = l4;
}

function updateDisplay() {
    if (setView) {
        /* Set view - show set selection */
        displayMessage(
            "SEQOMD - SELECT SET",
            currentSet >= 0 ? `Current: Set ${currentSet + 1}` : "",
            "",
            ""
        );
    } else if (masterMode) {
        /* Master view - show channels and sync */
        const chStr = tracks.map(t => String(t.channel + 1).padStart(2)).join("");

        displayMessage(
            "Track:  12345678",
            `Ch: ${chStr}`,
            `Sync: ${sendClock ? "ON" : "OFF"}`,
            ""
        );
    } else if (patternMode) {
        /* Pattern view - show which pattern each track is on + BPM */
        const patStr = tracks.map(t => String(t.currentPattern + 1)).join(" ");
        const viewRange = `${patternViewOffset + 1}-${patternViewOffset + 4}`;

        displayMessage(
            `PATTERNS ${viewRange}  ${bpm} BPM`,
            `Track:  12345678`,
            `Pattern: ${patStr}`,
            ""
        );
    } else if (shiftHeld) {
        /* Shift view - show channel and speed for current track */
        const trackNum = currentTrack + 1;
        const ch = tracks[currentTrack].channel + 1;
        const speedName = SPEED_OPTIONS[tracks[currentTrack].speedIndex].name;

        displayMessage(
            `Track ${trackNum}`,
            `Channel: ${ch}`,
            `Speed: ${speedName}`,
            ""
        );
    } else {
        /* Normal track view */
        const trackNum = currentTrack + 1;
        const muteStr = tracks[currentTrack].muted ? " [MUTE]" : "";
        const pattern = getCurrentPattern(currentTrack);
        const loopStr = (pattern.loopStart > 0 || pattern.loopEnd < NUM_STEPS - 1)
            ? `Loop:${pattern.loopStart + 1}-${pattern.loopEnd + 1}`
            : "";

        displayMessage(
            `Track ${trackNum}${muteStr}`,
            `Pattern ${tracks[currentTrack].currentPattern + 1}`,
            loopStr,
            ""
        );
    }
}

/* ============ LEDs ============ */

function updateStepLEDs() {
    if (setView) {
        /* Set view: steps off - focus is on pads */
        for (let i = 0; i < NUM_STEPS; i++) {
            setLED(MoveSteps[i], Black);
        }
    } else if (patternMode) {
        /* Pattern mode: steps off - focus is on pads */
        for (let i = 0; i < NUM_STEPS; i++) {
            setLED(MoveSteps[i], Black);
        }
    } else if (masterMode) {
        /* Master mode: steps could show master transpose (future) */
        for (let i = 0; i < NUM_STEPS; i++) {
            setLED(MoveSteps[i], LightGrey);
        }
    } else if (loopEditMode) {
        /* Loop edit mode: show loop points */
        const pattern = getCurrentPattern(currentTrack);
        const trackColor = TRACK_COLORS[currentTrack];

        for (let i = 0; i < NUM_STEPS; i++) {
            let color = Black;

            /* Steps in loop range */
            if (i >= pattern.loopStart && i <= pattern.loopEnd) {
                color = LightGrey;
            }

            /* Loop start/end markers */
            if (i === pattern.loopStart || i === pattern.loopEnd) {
                color = trackColor;
            }

            /* First selection while editing */
            if (loopEditFirst >= 0 && i === loopEditFirst) {
                color = Cyan;
            }

            setLED(MoveSteps[i], color);
        }
    } else if (sparkMode) {
        /* Spark mode: show selected steps in purple, others dim */
        const pattern = getCurrentPattern(currentTrack);

        for (let i = 0; i < NUM_STEPS; i++) {
            let color = Black;
            const step = pattern.steps[i];

            /* Steps with content show dim */
            if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
                color = LightGrey;
            }

            /* Steps with spark settings show cyan */
            if (step.paramSpark > 0 || step.compSpark > 0 || step.jump >= 0) {
                color = Cyan;
            }

            /* Selected steps in spark mode show purple (bright) */
            if (sparkSelectedSteps.has(i)) {
                color = Purple;
            }

            setLED(MoveSteps[i], color);
        }
    } else {
        /* Normal mode: show current track's pattern with playhead */
        const pattern = getCurrentPattern(currentTrack);
        const trackColor = TRACK_COLORS[currentTrack];
        const dimColor = TRACK_COLORS_DIM[currentTrack];

        for (let i = 0; i < NUM_STEPS; i++) {
            let color = Black;

            /* Steps outside loop range - very dim */
            const inLoop = i >= pattern.loopStart && i <= pattern.loopEnd;

            /* Step has content (notes or CC values) */
            const step = pattern.steps[i];
            if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
                color = inLoop ? dimColor : Navy;  /* Navy for out-of-loop content */
            }

            /* When holding a step, show its length tail */
            if (heldStep >= 0 && heldStep !== i) {
                const heldStepData = pattern.steps[heldStep];
                const heldLength = heldStepData.length || 1;  /* Safety: default to 1 */
                const lengthEnd = heldStep + heldLength - 1;
                if (i > heldStep && i <= lengthEnd) {
                    color = Cyan;  /* Show length tail in cyan */
                }
            }

            /* Playhead position - bright white (only when not holding a step) */
            if (playing && i === currentPlayStep && heldStep < 0) {
                color = White;
            }

            /* Held step - bright track color */
            if (i === heldStep) {
                color = trackColor;
            }

            setLED(MoveSteps[i], color);
        }
    }
}

function updatePadLEDs() {
    if (setView) {
        /* Set view: 32 pads = 32 sets
         * Layout: 4 rows x 8 columns
         * Row 4 (top, indices 0-7): Sets 25-32
         * Row 3 (indices 8-15): Sets 17-24
         * Row 2 (indices 16-23): Sets 9-16
         * Row 1 (bottom, indices 24-31): Sets 1-8
         */
        for (let i = 0; i < 32; i++) {
            const padNote = MovePads[i];
            /* Convert pad index to set index (bottom-left = set 0) */
            const row = 3 - Math.floor(i / 8);  // Row 0 is bottom
            const col = i % 8;
            const setIdx = row * 8 + col;

            const isCurrentSet = currentSet === setIdx;
            const hasContent = setHasContent(setIdx);

            let color = Black;
            if (isCurrentSet) {
                color = Cyan;  // Currently loaded set
            } else if (hasContent) {
                /* Use track colors cycling through for sets with content */
                color = TRACK_COLORS[setIdx % 8];
            }
            /* Empty sets stay Black */

            setLED(padNote, color);
        }
    } else if (patternMode) {
        /* Pattern mode: 8 columns (tracks) x 4 rows (patterns)
         * Uses patternViewOffset to show different pattern rows
         * Row 4 (top, indices 0-7): offset + 3
         * Row 3 (indices 8-15): offset + 2
         * Row 2 (indices 16-23): offset + 1
         * Row 1 (bottom, indices 24-31): offset + 0
         */
        for (let i = 0; i < 32; i++) {
            const padNote = MovePads[i];
            const trackIdx = i % 8;
            const rowIdx = 3 - Math.floor(i / 8);  // 0-3 from bottom
            const patternIdx = patternViewOffset + rowIdx;

            /* Check if pattern is in valid range */
            if (patternIdx >= NUM_PATTERNS) {
                setLED(padNote, Black);
                continue;
            }

            const isCurrentPattern = tracks[trackIdx].currentPattern === patternIdx;
            const patternHasContent = tracks[trackIdx].patterns[patternIdx].steps.some(s => s.notes.length > 0 || s.cc1 >= 0 || s.cc2 >= 0);

            let color = Black;
            if (isCurrentPattern) {
                color = TRACK_COLORS[trackIdx];  // Bright = current
            } else if (patternHasContent) {
                color = TRACK_COLORS_DIM[trackIdx];  // Dim = has content
            }

            setLED(padNote, color);
        }
    } else if (masterMode) {
        /* Master mode pad layout (MovePads[0-7] is TOP row):
         * Row 4 (top, indices 0-7): Reserved
         * Row 3 (indices 8-15): Reserved
         * Row 2 (indices 16-23): Track type for each track
         * Row 1 (bottom, indices 24-31): Chord follow toggle for each track
         * Pad order: leftmost pad in row = track 1
         */
        for (let i = 0; i < 32; i++) {
            const padNote = MovePads[i];
            if (i >= 24) {
                /* Row 1 (bottom): Chord follow toggles */
                const trackIdx = i - 24;  // Leftmost pad = track 1
                const followColor = masterData.followChord[trackIdx] ? TRACK_COLORS[trackIdx] : LightGrey;
                setLED(padNote, followColor);
            } else if (i >= 16) {
                /* Row 2: Track types */
                const trackIdx = i - 16;  // Leftmost pad = track 1
                setLED(padNote, TRACK_TYPE_COLORS[masterData.trackType[trackIdx]]);
            } else {
                /* Rows 3-4 (top): Reserved */
                setLED(padNote, Black);
            }
        }
    } else if (heldStep >= 0) {
        /* Holding a step: show only the notes that are in this step */
        const step = getCurrentPattern(currentTrack).steps[heldStep];
        const stepNotes = step.notes;
        const trackColor = TRACK_COLORS[currentTrack];

        for (let i = 0; i < 32; i++) {
            const padNote = MovePads[i];
            const midiNote = 36 + i;  // Pad 0 = MIDI 36, etc.

            if (stepNotes.includes(midiNote)) {
                /* Note is in this step - show bright */
                setLED(padNote, trackColor);
            } else {
                /* Note not in step - off */
                setLED(padNote, Black);
            }
        }
    } else {
        /* Normal mode: pads off (used for note input) */
        for (let i = 0; i < 32; i++) {
            setLED(MovePads[i], Black);
        }
    }
}

function updateTrackLEDs() {
    /* Track buttons (CC 40-43)
     * Button order is reversed: MoveTracks[0]=Row4 is rightmost, [3]=Row1 is leftmost
     * So we map: button 0 (leftmost/Row1) -> track 1, button 3 (rightmost/Row4) -> track 4
     */

    /* In pattern mode, show track colors for tracks 1-4 (or 5-8 with shift) */
    if (patternMode) {
        for (let i = 0; i < 4; i++) {
            const btnTrackOffset = 3 - i;
            const trackIdx = shiftHeld ? btnTrackOffset + 4 : btnTrackOffset;
            setButtonLED(MoveTracks[i], TRACK_COLORS[trackIdx]);
        }
        return;
    }

    /* Normal mode: show track selection */
    for (let i = 0; i < 4; i++) {
        /* Reverse the button index: leftmost button = track 1 */
        const btnTrackOffset = 3 - i;
        const trackIdx = shiftHeld ? btnTrackOffset + 4 : btnTrackOffset;
        let color = Black;

        if (trackIdx === currentTrack) {
            color = TRACK_COLORS[trackIdx];  // Bright = selected
        } else if (getCurrentPattern(trackIdx).steps.some(s => s.notes.length > 0 || s.cc1 >= 0 || s.cc2 >= 0)) {
            color = TRACK_COLORS_DIM[trackIdx];  // Dim color = has content
        }

        if (tracks[trackIdx].muted) {
            color = BrightRed;  // Muted indicator
        }

        setButtonLED(MoveTracks[i], color);
    }
}

function updateTransportLEDs() {
    setButtonLED(MovePlay, playing ? BrightGreen : Black);
    /* Loop LED: sync in master mode, loop status in track mode */
    if (masterMode) {
        setButtonLED(MoveLoop, sendClock ? Cyan : LightGrey);
    } else if (loopEditMode) {
        setButtonLED(MoveLoop, Cyan);  /* Editing loop */
    } else {
        /* Check if current track has custom loop (not full 0-15) */
        const pattern = getCurrentPattern(currentTrack);
        const hasCustomLoop = pattern.loopStart > 0 || pattern.loopEnd < NUM_STEPS - 1;
        setButtonLED(MoveLoop, hasCustomLoop ? TRACK_COLORS[currentTrack] : Black);
    }
    /* Record button - use CC like Play button */
    setButtonLED(MoveRec, recording ? BrightRed : Black);
}

function updateMasterButtonLED() {
    /* Menu button lights when in master mode (white LED uses brightness 0-127) */
    setButtonLED(MoveMenu, masterMode ? 127 : 0);
}

/* Set the indicator LED underneath a trigger button (uses 0xBA channel) */
function setTriggerIndicator(triggerCC, color) {
    move_midi_internal_send([0x0b, 0xBA, triggerCC, color]);
}

function updateTriggerIndicators() {
    /* Step 1 indicator: show in pattern mode when shift is held (to enter set view) */
    if (patternMode && shiftHeld) {
        setTriggerIndicator(MoveStep1UI, Cyan);  // Show indicator to enter set view
    } else {
        setTriggerIndicator(MoveStep1UI, Black);  // Hide indicator
    }
}

function updateKnobLEDs() {
    if (patternMode) {
        /* Pattern mode: all 8 knobs for CC */
        for (let i = 0; i < 8; i++) {
            setButtonLED(MoveKnobLEDs[i], Cyan);
        }
    } else if (masterMode) {
        /* Master mode: each knob shows its track color */
        for (let i = 0; i < 8; i++) {
            setButtonLED(MoveKnobLEDs[i], TRACK_COLORS[i]);
        }
    } else if (shiftHeld) {
        /* Shift mode: knobs 7-8 lit (speed/channel), others off */
        for (let i = 0; i < 6; i++) {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
        setButtonLED(MoveKnobLEDs[6], VividYellow);  /* Knob 7 = speed */
        setButtonLED(MoveKnobLEDs[7], Cyan);         /* Knob 8 = channel */
    } else if (heldStep >= 0) {
        /* Holding a step: show parameter status for this step */
        const step = getCurrentPattern(currentTrack).steps[heldStep];
        const trackColor = TRACK_COLORS[currentTrack];
        /* Knob 1 = CC1, Knob 2 = CC2 - lit if has value */
        setButtonLED(MoveKnobLEDs[0], step.cc1 >= 0 ? trackColor : LightGrey);
        setButtonLED(MoveKnobLEDs[1], step.cc2 >= 0 ? trackColor : LightGrey);
        /* Knobs 3-6 off */
        for (let i = 2; i < 6; i++) {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
        /* Knob 7 = Ratchet - lit if > 1 */
        setButtonLED(MoveKnobLEDs[6], step.ratchet > 0 ? trackColor : LightGrey);
        /* Knob 8 = Prob/Cond - lit if not default (100% / no condition) */
        const hasProb = step.probability < 100;
        const hasCond = step.condition > 0;
        setButtonLED(MoveKnobLEDs[7], (hasProb || hasCond) ? trackColor : LightGrey);
    } else {
        /* Track mode: knobs 1-2 lit with track color (CC output) */
        const trackColor = TRACK_COLORS[currentTrack];
        setButtonLED(MoveKnobLEDs[0], trackColor);
        setButtonLED(MoveKnobLEDs[1], trackColor);
        for (let i = 2; i < 8; i++) {
            setButtonLED(MoveKnobLEDs[i], Black);
        }
    }
}

function updateAllLEDs() {
    updateStepLEDs();
    updatePadLEDs();
    updateTrackLEDs();
    updateTransportLEDs();
    updateMasterButtonLED();
    updateKnobLEDs();
    updateTriggerIndicators();
}

/* ============ Helpers ============ */

function noteToName(n) {
    if (n <= 0) return "---";
    const names = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"];
    return names[n % 12] + (Math.floor(n/12) - 1);
}

/* Send CC to external MIDI (via DSP) */
function sendCCExternal(cc, value, channel) {
    host_module_set_param(`send_cc_${channel}_${cc}`, String(value));
}

/* Update CC value based on encoder movement and send it */
function updateAndSendCC(ccValues, index, velocity, cc, channel) {
    /* Encoder: 1-63 = clockwise (increment), 65-127 = counter-clockwise (decrement) */
    let val = ccValues[index];
    if (velocity >= 1 && velocity <= 63) {
        val = Math.min(val + 1, 127);  /* Fixed increment */
    } else if (velocity >= 65 && velocity <= 127) {
        val = Math.max(val - 1, 0);    /* Fixed decrement */
    }
    ccValues[index] = val;
    sendCCExternal(cc, val, channel);
    return val;
}

function notesToString(notes) {
    /* Format array of notes as string */
    if (!notes || notes.length === 0) return "---";
    return notes.map(n => noteToName(n)).join(" ");
}

function selectTrack(trackIdx) {
    if (trackIdx >= 0 && trackIdx < NUM_TRACKS) {
        currentTrack = trackIdx;
        updateDisplay();
        updateAllLEDs();
    }
}

function toggleTrackMute(trackIdx) {
    if (trackIdx >= 0 && trackIdx < NUM_TRACKS) {
        tracks[trackIdx].muted = !tracks[trackIdx].muted;
        host_module_set_param(`track_${trackIdx}_mute`, tracks[trackIdx].muted ? "1" : "0");
        updateDisplay();
        updateTrackLEDs();
    }
}

function toggleStepNote(trackIdx, stepIdx, note) {
    /* Toggle a note in/out of a step */
    const step = getCurrentPattern(trackIdx).steps[stepIdx];
    const stepNotes = step.notes;
    const noteIdx = stepNotes.indexOf(note);

    if (noteIdx >= 0) {
        /* Note exists - remove it */
        stepNotes.splice(noteIdx, 1);
        host_module_set_param(`track_${trackIdx}_step_${stepIdx}_remove_note`, String(note));
        return false;  // Note was removed
    } else {
        /* Note doesn't exist - add it (max 4) */
        if (stepNotes.length < 4) {
            stepNotes.push(note);
            host_module_set_param(`track_${trackIdx}_step_${stepIdx}_add_note`, String(note));
            return true;  // Note was added
        }
        return false;  // No room
    }
}

function clearStep(trackIdx, stepIdx) {
    /* Clear all notes, CCs, and step parameters */
    const step = getCurrentPattern(trackIdx).steps[stepIdx];
    step.notes = [];
    step.cc1 = -1;
    step.cc2 = -1;
    step.probability = 100;
    step.condition = 0;
    step.ratchet = 0;
    step.paramSpark = 0;
    step.compSpark = 0;
    step.jump = -1;
    host_module_set_param(`track_${trackIdx}_step_${stepIdx}_clear`, "1");
}

function setStepNote(trackIdx, stepIdx, note) {
    /* Set a single note (clears others) - for backward compat */
    if (note === 0) {
        clearStep(trackIdx, stepIdx);
    } else {
        const step = getCurrentPattern(trackIdx).steps[stepIdx];
        step.notes = [note];
        host_module_set_param(`track_${trackIdx}_step_${stepIdx}_note`, String(note));
    }
}

/* ============ Lifecycle ============ */

globalThis.init = function() {
    console.log("SEQOMD starting...");
    clearAllLEDs();

    /* Initialize 32 empty sets, then try to load from disk */
    for (let i = 0; i < NUM_SETS; i++) {
        sets[i] = null;  // null = empty/unused set
    }
    loadAllSetsFromDisk();  /* Load saved sets if they exist */

    /* Initialize track data with patterns */
    for (let t = 0; t < NUM_TRACKS; t++) {
        tracks[t] = {
            patterns: [],
            currentPattern: 0,
            muted: false,
            channel: t,
            speedIndex: DEFAULT_SPEED_INDEX
        };
        /* Initialize 8 patterns per track */
        for (let p = 0; p < NUM_PATTERNS; p++) {
            tracks[t].patterns.push({
                steps: [],
                loopStart: 0,
                loopEnd: NUM_STEPS - 1
            });
            for (let s = 0; s < NUM_STEPS; s++) {
                tracks[t].patterns[p].steps.push(createEmptyStep());
            }
        }
    }


    /* Enable clock output by default */
    host_module_set_param("send_clock", "1");
    sendClock = 1;

    currentTrack = 0;

    /* Start in set view mode */
    setView = true;
    currentSet = -1;  // No set loaded yet

    updateDisplay();
    updateAllLEDs();
};

/* Track which pads are lit for playback display */
let litPads = [];

globalThis.tick = function() {
    drawUI();

    /* Poll DSP for playhead position when playing (and not holding a step) */
    if (playing && heldStep < 0) {
        const stepStr = host_module_get_param(`track_${currentTrack}_current_step`);
        const newStep = stepStr ? parseInt(stepStr, 10) : -1;

        if (newStep !== currentPlayStep) {
            currentPlayStep = newStep;
            updateStepLEDs();

            /* Only update pad note display when NOT in pattern mode or master mode */
            if (!patternMode && !masterMode) {
                /* Clear previously lit pads */
                for (const padIdx of litPads) {
                    if (padIdx >= 0 && padIdx < 32) {
                        setLED(MovePads[padIdx], Black);
                    }
                }
                litPads = [];

                /* Light up pads for currently playing notes */
                if (currentPlayStep >= 0 && currentPlayStep < NUM_STEPS) {
                    const step = getCurrentPattern(currentTrack).steps[currentPlayStep];
                    const trackColor = TRACK_COLORS[currentTrack];

                    for (const note of step.notes) {
                        const padIdx = note - 36;
                        if (padIdx >= 0 && padIdx < 32) {
                            setLED(MovePads[padIdx], trackColor);
                            litPads.push(padIdx);
                        }
                    }
                }
            }
        }
    } else if (currentPlayStep !== -1 && !playing) {
        /* Stopped - clear playhead and note display */
        currentPlayStep = -1;
        lastRecordedStep = -1;
        for (const padIdx of litPads) {
            if (padIdx >= 0 && padIdx < 32) {
                setLED(MovePads[padIdx], Black);
            }
        }
        litPads = [];
        updateStepLEDs();
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

    /* Handle knob capacitive touch for tap-to-clear/toggle when holding a step */
    const touchKnobs = [MoveKnob1Touch, MoveKnob2Touch, MoveKnob7Touch, MoveKnob8Touch];
    const touchKnobIndices = [0, 1, 6, 7];  /* Actual knob indices */
    if (isNote && touchKnobs.includes(note)) {
        const touchIdx = touchKnobs.indexOf(note);
        const knobIdx = touchKnobIndices[touchIdx];
        if (heldStep >= 0) {
            if (isNoteOn && velocity > 0) {
                /* Knob touched - record time */
                knobTouchTime[knobIdx] = Date.now();
                knobTurned[knobIdx] = false;
            } else {
                /* Knob released - check if it was a tap (no turn) */
                if (knobTouchTime[knobIdx] && knobTouchTime[knobIdx] >= 0 && !knobTurned[knobIdx]) {
                    const touchDuration = Date.now() - knobTouchTime[knobIdx];
                    if (touchDuration < HOLD_THRESHOLD_MS) {
                        const step = getCurrentPattern(currentTrack).steps[heldStep];

                        if (knobIdx === 0 && step.cc1 >= 0) {
                            /* Tap knob 1: clear CC1 */
                            step.cc1 = -1;
                            host_module_set_param(`track_${currentTrack}_step_${heldStep}_cc1`, "-1");
                            displayMessage(undefined, `Step ${heldStep + 1}`, "CC1 cleared", "");
                        } else if (knobIdx === 1 && step.cc2 >= 0) {
                            /* Tap knob 2: clear CC2 */
                            step.cc2 = -1;
                            host_module_set_param(`track_${currentTrack}_step_${heldStep}_cc2`, "-1");
                            displayMessage(undefined, `Step ${heldStep + 1}`, "CC2 cleared", "");
                        } else if (knobIdx === 6 && step.ratchet > 0) {
                            /* Tap knob 7: reset ratchet to 1x */
                            step.ratchet = 0;
                            host_module_set_param(`track_${currentTrack}_step_${heldStep}_ratchet`, "1");
                            displayMessage(undefined, `Step ${heldStep + 1}`, "Ratchet: 1x", "");
                        } else if (knobIdx === 7) {
                            /* Tap knob 8: clear condition and reset probability */
                            step.condition = 0;
                            step.probability = 100;
                            host_module_set_param(`track_${currentTrack}_step_${heldStep}_condition_n`, "0");
                            host_module_set_param(`track_${currentTrack}_step_${heldStep}_condition_m`, "0");
                            host_module_set_param(`track_${currentTrack}_step_${heldStep}_condition_not`, "0");
                            host_module_set_param(`track_${currentTrack}_step_${heldStep}_probability`, "100");
                            displayMessage(undefined, `Step ${heldStep + 1}`, "Cond/Prob cleared", "");
                        }
                        updateKnobLEDs();
                        updateStepLEDs();
                    }
                }
                delete knobTouchTime[knobIdx];
            }
        }
        return;  /* Don't process further */
    }

    /* Filter other capacitive touch messages */
    if (isCapacitiveTouchMessage(data)) return;

    /* Shift button (CC 49) */
    if (isCC && note === MoveShift) {
        if (velocity > 0) {
            /* Shift pressed */
            if (sparkMode) {
                /* Exit spark mode */
                sparkMode = false;
                sparkSelectedSteps.clear();
                displayMessage("SEQOMD", `Track ${currentTrack + 1}`, "", "");
            }
            shiftHeld = true;
        } else {
            /* Shift released */
            shiftHeld = false;
        }
        updateTrackLEDs();
        updateStepLEDs();
        updateKnobLEDs();
        updateTriggerIndicators();
        updateDisplay();
        return;
    }

    /* Menu button (CC 50) - pattern mode or master mode when Shift held */
    if (isCC && note === MoveMenu) {
        if (velocity > 0) {
            if (shiftHeld) {
                /* Shift + Menu = master mode */
                masterMode = !masterMode;
                patternMode = false;
            } else {
                /* Menu alone = pattern mode */
                patternMode = !patternMode;
                masterMode = false;
            }
            updateDisplay();
            updateAllLEDs();
        }
        return;
    }

    /* Track buttons (CC 40-43) - triggers in pattern mode, track select otherwise */
    if (isCC && MoveTracks.includes(note)) {
        const btnIdx = MoveTracks.indexOf(note);
        /* Reverse the button index: leftmost button = track 1 */
        const trackBtnIdx = 3 - btnIdx;

        /* Normal track selection */
        if (velocity > 0) {
            /* Exit master mode when selecting a track */
            if (masterMode) {
                masterMode = false;
                updateMasterButtonLED();
            }
            const trackIdx = shiftHeld ? triggerIdx + 4 : triggerIdx;
            selectTrack(trackIdx);
        }
        return;
    }

    /* Step buttons (notes 16-31) */
    if (isNote && MoveSteps.includes(note)) {
        const stepIdx = MoveSteps.indexOf(note);

        /* In pattern mode: shift + step 1 = enter set view */
        if (patternMode && shiftHeld && stepIdx === 0 && isNoteOn && velocity > 0) {
            /* Save current set before going to set view */
            if (currentSet >= 0) {
                saveCurrentSet();
                saveAllSetsToDisk();
            }
            /* Enter set view */
            setView = true;
            patternMode = false;
            displayMessage(
                "SEQOMD - SELECT SET",
                currentSet >= 0 ? `Current: Set ${currentSet + 1}` : "",
                "",
                ""
            );
            updateAllLEDs();
            return;
        }

        /* Loop edit mode - set loop points */
        if (loopEditMode && isNoteOn && velocity > 0) {
            if (loopEditFirst < 0) {
                /* First step - set as start */
                loopEditFirst = stepIdx;
                displayMessage(
                    `Track ${currentTrack + 1} Loop`,
                    `Start: ${stepIdx + 1}`,
                    "Tap end step...",
                    ""
                );
            } else {
                /* Second step - set as end, apply loop */
                const startStep = Math.min(loopEditFirst, stepIdx);
                const endStep = Math.max(loopEditFirst, stepIdx);
                getCurrentPattern(currentTrack).loopStart = startStep;
                getCurrentPattern(currentTrack).loopEnd = endStep;
                host_module_set_param(`track_${currentTrack}_loop_start`, String(startStep));
                host_module_set_param(`track_${currentTrack}_loop_end`, String(endStep));
                displayMessage(
                    `Track ${currentTrack + 1} Loop`,
                    `Set: ${startStep + 1}-${endStep + 1}`,
                    `${endStep - startStep + 1} steps`,
                    ""
                );
                loopEditFirst = -1;  /* Ready for next selection */
                updateTransportLEDs();  /* Update loop button to show custom loop */
            }
            updateStepLEDs();
            return;
        }

        /* Spark mode: shift + step enters/toggles step selection */
        if (shiftHeld && isNoteOn && velocity > 0) {
            if (!sparkMode) {
                /* Enter spark mode */
                sparkMode = true;
                sparkSelectedSteps.clear();
            }
            /* Toggle step selection */
            if (sparkSelectedSteps.has(stepIdx)) {
                sparkSelectedSteps.delete(stepIdx);
            } else {
                sparkSelectedSteps.add(stepIdx);
            }
            /* Show spark mode display */
            const selectedCount = sparkSelectedSteps.size;
            const step = getCurrentPattern(currentTrack).steps[stepIdx];
            const paramCond = CONDITIONS[step.paramSpark || 0];
            const compCond = CONDITIONS[step.compSpark || 0];
            displayMessage(
                `SPARK MODE (${selectedCount})`,
                `Param: ${paramCond.name}`,
                `Comp: ${compCond.name}`,
                step.jump >= 0 ? `Jump: ${step.jump + 1}` : ""
            );
            updateStepLEDs();
            updateKnobLEDs();
            return;
        }

        /* Spark mode: tapping step without shift toggles selection */
        if (sparkMode && isNoteOn && velocity > 0) {
            if (sparkSelectedSteps.has(stepIdx)) {
                sparkSelectedSteps.delete(stepIdx);
            } else {
                sparkSelectedSteps.add(stepIdx);
            }
            const selectedCount = sparkSelectedSteps.size;
            if (selectedCount > 0) {
                /* Show first selected step's spark values */
                const firstStep = [...sparkSelectedSteps][0];
                const step = getCurrentPattern(currentTrack).steps[firstStep];
                const paramCond = CONDITIONS[step.paramSpark || 0];
                const compCond = CONDITIONS[step.compSpark || 0];
                displayMessage(
                    `SPARK MODE (${selectedCount})`,
                    `Param: ${paramCond.name}`,
                    `Comp: ${compCond.name}`,
                    step.jump >= 0 ? `Jump: ${step.jump + 1}` : ""
                );
            } else {
                displayMessage("SPARK MODE", "No steps selected", "", "");
            }
            updateStepLEDs();
            return;
        }

        if (isNoteOn && velocity > 0) {
            /* Check if we're holding another step - set length */
            if (heldStep >= 0 && heldStep !== stepIdx && stepIdx > heldStep) {
                const step = getCurrentPattern(currentTrack).steps[heldStep];
                const newLength = stepIdx - heldStep + 1;
                const currentLength = step.length || 1;  /* Safety: default to 1 */

                /* If pressing the current length end, toggle back to 1 */
                if (currentLength === newLength) {
                    step.length = 1;
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_length`, "1");
                    displayMessage(
                        `Step ${heldStep + 1}`,
                        `Length: 1 step`,
                        "",
                        ""
                    );
                } else {
                    /* Set new length */
                    step.length = newLength;
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_length`, String(newLength));
                    displayMessage(
                        `Step ${heldStep + 1}`,
                        `Length: ${newLength} steps`,
                        `→ Step ${stepIdx + 1}`,
                        ""
                    );
                }
                stepPadPressed[heldStep] = true;  /* Mark as modified */
                updateStepLEDs();
                return;  /* Don't change held step */
            }

            /* Step pressed - record time and show on pads */
            setLED(note, TRACK_COLORS[currentTrack]);
            stepPressTimes[stepIdx] = Date.now();
            stepPadPressed[stepIdx] = false;
            heldStep = stepIdx;
            updatePadLEDs();  /* Show notes in this step on pads */
            updateKnobLEDs(); /* Show CC status for this step */
            updateStepLEDs(); /* Show length if any */
        } else {
            /* Step released - check per-step tracking */
            const pressTime = stepPressTimes[stepIdx];
            const padPressed = stepPadPressed[stepIdx];

            if (pressTime !== undefined && !padPressed) {
                const holdDuration = Date.now() - pressTime;
                if (holdDuration < HOLD_THRESHOLD_MS) {
                    /* Quick tap - clear if has data, add note if empty */
                    const step = getCurrentPattern(currentTrack).steps[stepIdx];
                    if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
                        clearStep(currentTrack, stepIdx);
                    } else if (lastSelectedNote > 0) {
                        toggleStepNote(currentTrack, stepIdx, lastSelectedNote);
                    }
                }
            }

            /* Clean up per-step tracking */
            delete stepPressTimes[stepIdx];
            delete stepPadPressed[stepIdx];

            /* Update heldStep if this was the most recent */
            if (heldStep === stepIdx) {
                heldStep = -1;
                updatePadLEDs();
                updateKnobLEDs();
            }
            updateStepLEDs();
        }
        return;
    }

    /* Pads (notes 68-99) */
    if (isNote && MovePads.includes(note)) {
        const padIdx = MovePads.indexOf(note);

        if (setView) {
            /* Set view: select a set */
            if (isNoteOn && velocity > 0) {
                /* Convert pad index to set index (bottom-left = set 0) */
                const row = 3 - Math.floor(padIdx / 8);  // Row 0 is bottom
                const col = padIdx % 8;
                const setIdx = row * 8 + col;

                /* Save current set if we have one loaded */
                if (currentSet >= 0) {
                    saveCurrentSet();
                    saveAllSetsToDisk();
                }

                /* Load the selected set */
                loadSetToTracks(setIdx);

                /* Exit set view */
                setView = false;

                displayMessage(
                    `SEQOMD - Set ${setIdx + 1}`,
                    `Track ${currentTrack + 1}`,
                    "",
                    ""
                );
                updateAllLEDs();
            }
        } else if (patternMode) {
            /* Pattern mode: select pattern for track */
            if (isNoteOn && velocity > 0) {
                const trackIdx = padIdx % 8;
                const rowIdx = 3 - Math.floor(padIdx / 8);  // 0-3 from bottom
                const patternIdx = patternViewOffset + rowIdx;

                /* Only select if pattern is in valid range */
                if (patternIdx < NUM_PATTERNS) {
                    tracks[trackIdx].currentPattern = patternIdx;
                    host_module_set_param(`track_${trackIdx}_pattern`, String(patternIdx));

                    displayMessage(
                        `PATTERNS      ${bpm} BPM`,
                        `Track ${trackIdx + 1} -> Pat ${patternIdx + 1}`,
                        "",
                        ""
                    );
                    updatePadLEDs();
                }
            }
        } else if (masterMode) {
            /* Master mode pad handling (indices 24-31 = row 1 bottom, 16-23 = row 2) */
            if (isNoteOn && velocity > 0) {
                if (padIdx >= 24) {
                    /* Row 1 (bottom): Toggle chord follow for track */
                    const trackIdx = padIdx - 24;  // Leftmost = track 1
                    masterData.followChord[trackIdx] = !masterData.followChord[trackIdx];
                    host_module_set_param(`track_${trackIdx}_follow_chord`,
                        masterData.followChord[trackIdx] ? "1" : "0");
                    updateDisplay();
                    updatePadLEDs();
                } else if (padIdx >= 16) {
                    /* Row 2: Cycle track type */
                    const trackIdx = padIdx - 16;  // Leftmost = track 1
                    masterData.trackType[trackIdx] = (masterData.trackType[trackIdx] + 1) % 4;
                    host_module_set_param(`track_${trackIdx}_type`,
                        String(masterData.trackType[trackIdx]));
                    updateDisplay();
                    updatePadLEDs();
                }
            }
        } else if (heldStep >= 0) {
            /* Holding a step: toggle notes */
            const midiNote = 36 + padIdx;

            if (isNoteOn && velocity > 0) {
                /* Toggle note in step */
                const wasAdded = toggleStepNote(currentTrack, heldStep, midiNote);
                stepPadPressed[heldStep] = true;

                /* Preview the note */
                host_module_set_param(`track_${currentTrack}_preview_note`, String(midiNote));

                /* Update display and pad LEDs */
                const step = getCurrentPattern(currentTrack).steps[heldStep];
                displayMessage(
                    undefined,
                    `Step ${heldStep + 1}`,
                    `Notes: ${notesToString(step.notes)}`,
                    wasAdded ? `Added ${noteToName(midiNote)}` : `Removed ${noteToName(midiNote)}`
                );
                updatePadLEDs();
                updateStepLEDs();
            } else if (!isNoteOn || velocity === 0) {
                /* Pad released - stop preview */
                host_module_set_param(`track_${currentTrack}_preview_note_off`, String(midiNote));
            }
        } else {
            /* No step held: select this note for quick step entry */
            const midiNote = 36 + padIdx;

            if (isNoteOn && velocity > 0) {
                lastSelectedNote = midiNote;
                heldPads.add(midiNote);  /* Track for recording */
                host_module_set_param(`track_${currentTrack}_preview_note`, String(midiNote));
                setLED(note, TRACK_COLORS[currentTrack]);

                /* If recording and playing, immediately add to current step */
                if (recording && playing && currentPlayStep >= 0) {
                    const step = getCurrentPattern(currentTrack).steps[currentPlayStep];
                    if (!step.notes.includes(midiNote) && step.notes.length < 4) {
                        step.notes.push(midiNote);
                        host_module_set_param(`track_${currentTrack}_step_${currentPlayStep}_add_note`, String(midiNote));
                    }
                    lastRecordedStep = currentPlayStep;  /* Mark this step as recorded */
                }

            } else if (!isNoteOn || velocity === 0) {
                heldPads.delete(midiNote);  /* Track for recording */
                host_module_set_param(`track_${currentTrack}_preview_note_off`, String(midiNote));
                setLED(note, Black);
                /* Note stays selected (lastSelectedNote) for step entry */
            }
        }
        return;
    }

    /* Loop button - toggle clock in master mode, edit loop points in track mode */
    if (isCC && note === MoveLoop) {
        if (velocity > 0) {
            if (masterMode) {
                /* Toggle clock output in master mode */
                sendClock = sendClock ? 0 : 1;
                host_module_set_param("send_clock", String(sendClock));
                updateDisplay();
                updateTransportLEDs();
            } else {
                /* Enter loop edit mode in track mode */
                loopEditMode = true;
                loopEditFirst = -1;
                const pattern = getCurrentPattern(currentTrack);
                displayMessage(
                    `Track ${currentTrack + 1} Loop`,
                    `Current: ${pattern.loopStart + 1}-${pattern.loopEnd + 1}`,
                    "Tap start step...",
                    ""
                );
                updateStepLEDs();
            }
        } else {
            /* Loop released - exit loop edit mode */
            if (loopEditMode) {
                loopEditMode = false;
                loopEditFirst = -1;
                updateDisplay();
                updateStepLEDs();
                updateTransportLEDs();  /* Show loop status (track color if custom) */
            }
        }
        return;
    }

    /* Play button (CC 85) - toggle play */
    if (isCC && note === MovePlay) {
        if (velocity > 0) {
            playing = !playing;
            host_module_set_param("playing", playing ? "1" : "0");
            if (!playing) {
                lastRecordedStep = -1;  /* Reset recording position */
            }
            updateDisplay();
            updateTransportLEDs();
        }
        return;
    }

    /* Record button (CC 86) - toggle recording mode */
    if (isCC && note === MoveRec) {
        if (velocity > 0) {
            recording = !recording;
            lastRecordedStep = -1;  /* Reset so we record on next step */
            updateAllLEDs();  /* Make sure record LED updates */
        }
        return;
    }

    /* Knobs (CC 71-78) */
    const knobs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];
    if (isCC && knobs.includes(note)) {
        const knobIdx = knobs.indexOf(note);

        if (shiftHeld) {
            /* Shift + Knob 7 = change speed for current track */
            if (knobIdx === 6) {
                let speedIdx = tracks[currentTrack].speedIndex;
                if (velocity >= 1 && velocity <= 63) {
                    speedIdx = Math.min(speedIdx + 1, SPEED_OPTIONS.length - 1);
                } else if (velocity >= 65 && velocity <= 127) {
                    speedIdx = Math.max(speedIdx - 1, 0);
                }
                tracks[currentTrack].speedIndex = speedIdx;
                const speedMult = SPEED_OPTIONS[speedIdx].mult;
                host_module_set_param(`track_${currentTrack}_speed`, String(speedMult));
                updateDisplay();
            }
            /* Shift + Knob 8 = change MIDI channel for current track */
            else if (knobIdx === 7) {
                let channel = tracks[currentTrack].channel;
                if (velocity >= 1 && velocity <= 63) {
                    channel = (channel + 1) % 16;
                } else if (velocity >= 65 && velocity <= 127) {
                    channel = (channel - 1 + 16) % 16;
                }
                tracks[currentTrack].channel = channel;
                host_module_set_param(`track_${currentTrack}_channel`, String(channel));
                updateDisplay();
            }
        } else if (sparkMode && sparkSelectedSteps.size > 0 && (knobIdx === 0 || knobIdx === 6 || knobIdx === 7)) {
            /* Spark mode: knob 1 = jump, knob 7 = comp spark, knob 8 = param spark */
            const pattern = getCurrentPattern(currentTrack);

            for (const stepIdx of sparkSelectedSteps) {
                const step = pattern.steps[stepIdx];

                if (knobIdx === 0) {
                    /* Knob 1: Jump target (0-15, or -1 for off) */
                    let jump = step.jump;
                    if (velocity >= 1 && velocity <= 63) {
                        jump = Math.min(jump + 1, NUM_STEPS - 1);
                    } else if (velocity >= 65 && velocity <= 127) {
                        jump = Math.max(jump - 1, -1);
                    }
                    step.jump = jump;
                    host_module_set_param(`track_${currentTrack}_step_${stepIdx}_jump`, String(jump));
                } else if (knobIdx === 6) {
                    /* Knob 7: Component Spark (when ratchet/jump apply) */
                    let compSpark = step.compSpark || 0;
                    if (velocity >= 1 && velocity <= 63) {
                        compSpark = Math.min(compSpark + 1, CONDITIONS.length - 1);
                    } else if (velocity >= 65 && velocity <= 127) {
                        compSpark = Math.max(compSpark - 1, 0);
                    }
                    step.compSpark = compSpark;
                    const cond = CONDITIONS[compSpark];
                    host_module_set_param(`track_${currentTrack}_step_${stepIdx}_comp_spark_n`, String(cond.n));
                    host_module_set_param(`track_${currentTrack}_step_${stepIdx}_comp_spark_m`, String(cond.m));
                    host_module_set_param(`track_${currentTrack}_step_${stepIdx}_comp_spark_not`, cond.not ? "1" : "0");
                } else if (knobIdx === 7) {
                    /* Knob 8: Parameter Spark (when CC locks apply) */
                    let paramSpark = step.paramSpark || 0;
                    if (velocity >= 1 && velocity <= 63) {
                        paramSpark = Math.min(paramSpark + 1, CONDITIONS.length - 1);
                    } else if (velocity >= 65 && velocity <= 127) {
                        paramSpark = Math.max(paramSpark - 1, 0);
                    }
                    step.paramSpark = paramSpark;
                    const cond = CONDITIONS[paramSpark];
                    host_module_set_param(`track_${currentTrack}_step_${stepIdx}_param_spark_n`, String(cond.n));
                    host_module_set_param(`track_${currentTrack}_step_${stepIdx}_param_spark_m`, String(cond.m));
                    host_module_set_param(`track_${currentTrack}_step_${stepIdx}_param_spark_not`, cond.not ? "1" : "0");
                }
            }

            /* Update display with first selected step */
            const firstStep = [...sparkSelectedSteps][0];
            const step = pattern.steps[firstStep];
            const paramCond = CONDITIONS[step.paramSpark || 0];
            const compCond = CONDITIONS[step.compSpark || 0];
            displayMessage(
                `SPARK MODE (${sparkSelectedSteps.size})`,
                `Param: ${paramCond.name}`,
                `Comp: ${compCond.name}`,
                step.jump >= 0 ? `Jump: ${step.jump + 1}` : "Jump: ---"
            );
            updateStepLEDs();
        } else if (patternMode) {
            /* Pattern mode: all 8 knobs send CCs 1-8 on master channel */
            const cc = knobIdx + 1;  // CC 1-8
            const val = updateAndSendCC(patternCCValues, knobIdx, velocity, cc, MASTER_CC_CHANNEL);
            displayMessage(
                "PATTERNS",
                `Knob ${knobIdx + 1}: CC ${cc}`,
                `Value: ${val}`,
                ""
            );
        } else if (masterMode) {
            /* Master mode: each knob changes MIDI channel for that track */
            /* Knob 1 (leftmost) = track 1, knob 8 (rightmost) = track 8 */
            const trackIdx = knobIdx;
            let channel = tracks[trackIdx].channel;
            if (velocity >= 1 && velocity <= 63) {
                channel = (channel + 1) % 16;
            } else if (velocity >= 65 && velocity <= 127) {
                channel = (channel - 1 + 16) % 16;
            }
            tracks[trackIdx].channel = channel;
            host_module_set_param(`track_${trackIdx}_channel`, String(channel));
            updateDisplay();
        } else if (heldStep >= 0 && (knobIdx < 2 || knobIdx === 6 || knobIdx === 7)) {
            /* Holding a step: knobs 1-2 = CC, knob 7 = ratchet, knob 8 = prob/cond */
            const step = getCurrentPattern(currentTrack).steps[heldStep];
            /* Mark as turned (not a tap) for tap detection */
            knobTurned[knobIdx] = true;

            if (knobIdx < 2) {
                /* Knobs 1-2: CC values */
                let val = knobIdx === 0 ? step.cc1 : step.cc2;
                if (val < 0) val = 64;  /* Default to center */

                if (velocity >= 1 && velocity <= 63) {
                    val = Math.min(val + 1, 127);  /* Fixed increment */
                } else if (velocity >= 65 && velocity <= 127) {
                    val = Math.max(val - 1, 0);    /* Fixed decrement */
                }

                if (knobIdx === 0) {
                    step.cc1 = val;
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_cc1`, String(val));
                } else {
                    step.cc2 = val;
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_cc2`, String(val));
                }

                const cc = 20 + (currentTrack * 2) + knobIdx;
                displayMessage(
                    `Step ${heldStep + 1}`,
                    `CC${knobIdx + 1}: ${val}`,
                    `(CC ${cc} on Ch ${tracks[currentTrack].channel + 1})`,
                    ""
                );
            } else if (knobIdx === 6) {
                /* Knob 7: Ratchet */
                let ratchIdx = step.ratchet;
                if (velocity >= 1 && velocity <= 63) {
                    ratchIdx = Math.min(ratchIdx + 1, RATCHET_VALUES.length - 1);
                } else if (velocity >= 65 && velocity <= 127) {
                    ratchIdx = Math.max(ratchIdx - 1, 0);
                }
                step.ratchet = ratchIdx;
                host_module_set_param(`track_${currentTrack}_step_${heldStep}_ratchet`, String(RATCHET_VALUES[ratchIdx]));

                displayMessage(
                    `Step ${heldStep + 1}`,
                    `Ratchet: ${RATCHET_VALUES[ratchIdx]}x`,
                    "",
                    ""
                );
            } else if (knobIdx === 7) {
                /* Knob 8: Probability (down) / Condition (up) */
                if (velocity >= 65 && velocity <= 127) {
                    /* Turning down = decrease probability in 5% steps */
                    step.condition = 0;  /* Clear condition when using probability */
                    step.conditionNot = false;
                    step.probability = Math.max(step.probability - 5, 5);  /* Min 5%, step by 5 */
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_probability`, String(step.probability));
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_condition_n`, "0");
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_condition_m`, "0");

                    displayMessage(
                        `Step ${heldStep + 1}`,
                        `Probability: ${step.probability}%`,
                        "",
                        ""
                    );
                } else if (velocity >= 1 && velocity <= 63) {
                    /* Turning up = cycle through conditions (including inverted) */
                    step.probability = 100;  /* Reset probability when using condition */
                    step.condition = Math.min(step.condition + 1, CONDITIONS.length - 1);

                    const cond = CONDITIONS[step.condition];
                    /* Send condition params to DSP */
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_probability`, "100");
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_condition_n`, String(cond.n));
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_condition_m`, String(cond.m));
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_condition_not`, cond.not ? "1" : "0");

                    /* Format display */
                    let desc = "Always";
                    if (step.condition > 0) {
                        if (cond.not) {
                            desc = `Skip loop ${cond.m} of ${cond.n}`;
                        } else {
                            desc = `Play on loop ${cond.m} of ${cond.n}`;
                        }
                    }

                    displayMessage(
                        `Step ${heldStep + 1}`,
                        `Condition: ${cond.name}`,
                        desc,
                        ""
                    );
                }
            }
            updateKnobLEDs();
        } else {
            /* Track mode: knobs 1-2 send CCs on track's channel */
            if (knobIdx < 2) {
                /* CC number: 20 + (track * 2) + knobIdx
                 * Track 0: CC 20, 21
                 * Track 1: CC 22, 23
                 * ...
                 * Track 7: CC 34, 35
                 */
                const cc = 20 + (currentTrack * 2) + knobIdx;
                const ccValIdx = currentTrack * 2 + knobIdx;
                const channel = tracks[currentTrack].channel;
                const val = updateAndSendCC(trackCCValues, ccValIdx, velocity, cc, channel);
                displayMessage(
                    `Track ${currentTrack + 1}`,
                    `Knob ${knobIdx + 1}: CC ${cc}`,
                    `Value: ${val}  Ch: ${channel + 1}`,
                    ""
                );
            }
        }
        return;
    }

    /* Main knob touch (note 8) */
    if (isNote && note === MoveMasterTouch) {
        mainKnobTouched = isNoteOn && velocity > 0;
        return;
    }

    /* Main knob / jog wheel turn (CC 14) */
    if (isCC && note === MoveMainKnob) {
        if (patternMode) {
            if (mainKnobTouched) {
                /* Touched + turn = BPM control */
                if (velocity >= 1 && velocity <= 63) {
                    bpm = Math.min(bpm + 1, 300);
                } else if (velocity >= 65 && velocity <= 127) {
                    bpm = Math.max(bpm - 1, 20);
                }
                host_module_set_param("bpm", String(bpm));
                displayMessage(
                    `PATTERNS      ${bpm} BPM`,
                    `Track:  12345678`,
                    `Pattern: ${tracks.map(t => String(t.currentPattern + 1)).join(" ")}`,
                    ""
                );
            } else {
                /* Not touched = scroll pattern view */
                if (velocity >= 1 && velocity <= 63) {
                    patternViewOffset = Math.min(patternViewOffset + 4, NUM_PATTERNS - 4);
                } else if (velocity >= 65 && velocity <= 127) {
                    patternViewOffset = Math.max(patternViewOffset - 4, 0);
                }
                updateDisplay();
                updatePadLEDs();
            }
        }
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {};
