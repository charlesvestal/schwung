/*
 * SEQOMD UI
 * 8 tracks with track selection via track buttons + shift
 * Move is master - sends MIDI clock to external devices
 */

import {
    Black, White, LightGrey, Navy, BrightGreen, Cyan, BrightRed,
    OrangeRed, VividYellow, RoyalBlue, Purple,
    MidiNoteOn, MidiNoteOff, MidiCC,
    MovePlay, MoveLoop, MoveSteps, MovePads, MoveTracks, MoveShift, MoveMenu, MoveRec, MoveRecord,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveKnob1Touch, MoveKnob2Touch, MoveKnob7Touch, MoveKnob8Touch
} from "../../shared/constants.mjs";

import {
    isNoiseMessage, isCapacitiveTouchMessage,
    setLED, setButtonLED, clearAllLEDs
} from "../../shared/input_filter.mjs";

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
    { name: "---", n: 0, m: 0 },   // No condition (use probability)
    // Every 2 loops
    { name: "1:2", n: 2, m: 1 },   // Play on 1st of every 2 loops
    { name: "2:2", n: 2, m: 2 },
    // Every 3 loops
    { name: "1:3", n: 3, m: 1 },
    { name: "2:3", n: 3, m: 2 },
    { name: "3:3", n: 3, m: 3 },
    // Every 4 loops
    { name: "1:4", n: 4, m: 1 },
    { name: "2:4", n: 4, m: 2 },
    { name: "3:4", n: 4, m: 3 },
    { name: "4:4", n: 4, m: 4 },
    // Every 5 loops
    { name: "1:5", n: 5, m: 1 },
    { name: "2:5", n: 5, m: 2 },
    { name: "3:5", n: 5, m: 3 },
    { name: "4:5", n: 5, m: 4 },
    { name: "5:5", n: 5, m: 5 },
    // Every 6 loops
    { name: "1:6", n: 6, m: 1 },
    { name: "2:6", n: 6, m: 2 },
    { name: "3:6", n: 6, m: 3 },
    { name: "4:6", n: 6, m: 4 },
    { name: "5:6", n: 6, m: 5 },
    { name: "6:6", n: 6, m: 6 },
    // Every 8 loops
    { name: "1:8", n: 8, m: 1 },
    { name: "2:8", n: 8, m: 2 },
    { name: "3:8", n: 8, m: 3 },
    { name: "4:8", n: 8, m: 4 },
    { name: "5:8", n: 8, m: 5 },
    { name: "6:8", n: 8, m: 6 },
    { name: "7:8", n: 8, m: 7 },
    { name: "8:8", n: 8, m: 8 }
];

/* Create a new empty step */
function createEmptyStep() {
    return {
        notes: [],      // Array of MIDI notes
        cc1: -1,        // CC value for knob 1 (-1 = not set)
        cc2: -1,        // CC value for knob 2 (-1 = not set)
        probability: 100,  // 1-100%
        condition: 0,      // Index into CONDITIONS (0 = none)
        ratchet: 0         // Index into RATCHET_VALUES (0 = 1x, no ratchet)
    };
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
    if (masterMode) {
        /* Master view - show channels and sync */
        const chStr = tracks.map(t => String(t.channel + 1).padStart(2)).join("");

        displayMessage(
            "Track:  12345678",
            `Ch: ${chStr}`,
            `Sync: ${sendClock ? "ON" : "OFF"}`,
            ""
        );
    } else if (patternMode) {
        /* Pattern view - show which pattern each track is on */
        const patStr = tracks.map(t => String(t.currentPattern + 1)).join(" ");

        displayMessage(
            "PATTERNS 12345678",
            `Pattern: ${patStr}`,
            "Tap pad to select",
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
            "K7:Speed K8:Channel"
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
    if (patternMode) {
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
    } else {
        /* Normal mode: show current track's pattern with playhead */
        const pattern = getCurrentPattern(currentTrack);
        const trackColor = TRACK_COLORS[currentTrack];
        const dimColor = TRACK_COLORS_DIM[currentTrack];

        /* Build a map of which steps are "tails" of longer notes */
        const lengthTails = new Set();
        for (let s = 0; s < NUM_STEPS; s++) {
            const step = pattern.steps[s];
            const stepLength = step.length || 1;  /* Safety: default to 1 */
            if (stepLength > 1 && step.notes && step.notes.length > 0) {
                for (let t = 1; t < stepLength && (s + t) < NUM_STEPS; t++) {
                    lengthTails.add(s + t);
                }
            }
        }

        for (let i = 0; i < NUM_STEPS; i++) {
            let color = Black;

            /* Steps outside loop range - very dim */
            const inLoop = i >= pattern.loopStart && i <= pattern.loopEnd;

            /* Check if this step is a "tail" of a longer note */
            if (lengthTails.has(i)) {
                color = inLoop ? Cyan : Navy;  /* Cyan for length tails */
            }

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
    if (patternMode) {
        /* Pattern mode: 8 columns (tracks) x 4 rows (patterns 1-4)
         * Row 4 (top, indices 0-7): Pattern 4
         * Row 3 (indices 8-15): Pattern 3
         * Row 2 (indices 16-23): Pattern 2
         * Row 1 (bottom, indices 24-31): Pattern 1
         */
        for (let i = 0; i < 32; i++) {
            const padNote = MovePads[i];
            const trackIdx = i % 8;
            const patternIdx = 3 - Math.floor(i / 8);  // Row 1 = pattern 0, Row 4 = pattern 3

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

function updateKnobLEDs() {
    if (patternMode) {
        /* Pattern mode: all 8 knobs lit with a subtle color */
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
                            /* Tap knob 8: clear the entire step */
                            clearStep(currentTrack, heldStep);
                            displayMessage(undefined, `Step ${heldStep + 1}`, "Step cleared", "");
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
        shiftHeld = velocity > 0;
        updateTrackLEDs();  // Update to show tracks 5-8 when shift held
        updateKnobLEDs();   // Show knob 7/8 functions
        updateDisplay();    // Show shift view with channel/speed
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

    /* Track buttons (CC 40-43) */
    if (isCC && MoveTracks.includes(note)) {
        const btnIdx = MoveTracks.indexOf(note);
        /* Reverse the button index: leftmost button = track 1 */
        const btnTrackOffset = 3 - btnIdx;

        if (velocity > 0) {
            /* Exit master mode when selecting a track */
            if (masterMode) {
                masterMode = false;
                updateMasterButtonLED();
            }
            const trackIdx = shiftHeld ? btnTrackOffset + 4 : btnTrackOffset;
            selectTrack(trackIdx);
        }
        return;
    }

    /* Step buttons (notes 16-31) */
    if (isNote && MoveSteps.includes(note)) {
        const stepIdx = MoveSteps.indexOf(note);

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

        if (patternMode) {
            /* Pattern mode: select pattern for track */
            if (isNoteOn && velocity > 0) {
                const trackIdx = padIdx % 8;
                const patternIdx = 3 - Math.floor(padIdx / 8);  // Row 1 = pattern 0, etc.

                tracks[trackIdx].currentPattern = patternIdx;
                host_module_set_param(`track_${trackIdx}_pattern`, String(patternIdx));

                displayMessage(
                    "PATTERNS 12345678",
                    `Track ${trackIdx + 1} -> Pat ${patternIdx + 1}`,
                    "",
                    ""
                );
                updatePadLEDs();
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
                    "Tap to reset",
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
                        "Tap to clear step",
                        ""
                    );
                } else if (velocity >= 1 && velocity <= 63) {
                    /* Turning up = cycle through conditions */
                    step.probability = 100;  /* Reset probability when using condition */
                    step.condition = Math.min(step.condition + 1, CONDITIONS.length - 1);

                    const cond = CONDITIONS[step.condition];
                    /* Send condition params to DSP */
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_probability`, "100");
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_condition_n`, String(cond.n));
                    host_module_set_param(`track_${currentTrack}_step_${heldStep}_condition_m`, String(cond.m));

                    /* Format display */
                    let desc = "Always";
                    if (step.condition > 0) {
                        desc = `Play on loop ${cond.m} of ${cond.n}`;
                    }

                    displayMessage(
                        `Step ${heldStep + 1}`,
                        `Condition: ${cond.name}`,
                        desc,
                        "Tap to clear step"
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
};

globalThis.onMidiMessageExternal = function(data) {};
