/*
 * Multi-Track Step Sequencer UI
 * 8 tracks with track selection via track buttons + shift
 * Move is master - sends MIDI clock to external devices
 */

import {
    Black, White, LightGrey, Navy, BrightGreen, Cyan, BrightRed,
    OrangeRed, VividYellow, RoyalBlue, Purple,
    MidiNoteOn, MidiNoteOff, MidiCC,
    MovePlay, MoveLoop, MoveSteps, MovePads, MoveTracks, MoveShift, MoveMenu, MoveRec, MoveRecord,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8
} from "../../shared/constants.mjs";

import {
    isNoiseMessage, isCapacitiveTouchMessage,
    setLED, setButtonLED, clearAllLEDs
} from "../../shared/input_filter.mjs";

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

const HOLD_THRESHOLD_MS = 300;  // Time threshold for hold vs tap

/* Per-track step data (mirrors DSP state) */
const NUM_PATTERNS = 8;
let tracks = [];
for (let t = 0; t < NUM_TRACKS; t++) {
    tracks.push({
        patterns: [],  // Array of patterns, each pattern has steps
        currentPattern: 0,
        muted: false,
        channel: t
    });
    /* Initialize 8 patterns per track */
    for (let p = 0; p < NUM_PATTERNS; p++) {
        tracks[t].patterns.push({
            steps: [],
            loopStart: 0,
            loopEnd: NUM_STEPS - 1
        });
        for (let s = 0; s < NUM_STEPS; s++) {
            tracks[t].patterns[p].steps.push([]);  // Empty array of notes
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
let line1 = "Multi-Track Seq";
let line2 = "Track 1: Kick";
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
    } else {
        /* Normal track view */
        const trackNum = currentTrack + 1;
        const typeName = TRACK_TYPE_NAMES[masterData.trackType[currentTrack]];
        const muteStr = tracks[currentTrack].muted ? " [MUTE]" : "";
        const pattern = getCurrentPattern(currentTrack);
        const loopStr = (pattern.loopStart > 0 || pattern.loopEnd < NUM_STEPS - 1)
            ? `Loop:${pattern.loopStart + 1}-${pattern.loopEnd + 1}`
            : "";

        displayMessage(
            `Track ${trackNum}: ${typeName}${muteStr}`,
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

        for (let i = 0; i < NUM_STEPS; i++) {
            let color = Black;

            /* Steps outside loop range - very dim */
            const inLoop = i >= pattern.loopStart && i <= pattern.loopEnd;

            /* Step has content (array has notes) */
            if (pattern.steps[i].length > 0) {
                color = inLoop ? dimColor : Navy;  /* Navy for out-of-loop content */
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
            const patternHasContent = tracks[trackIdx].patterns[patternIdx].steps.some(s => s.length > 0);

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
        const stepNotes = getCurrentPattern(currentTrack).steps[heldStep];
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
        } else if (getCurrentPattern(trackIdx).steps.some(s => s.length > 0)) {
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

function updateAllLEDs() {
    updateStepLEDs();
    updatePadLEDs();
    updateTrackLEDs();
    updateTransportLEDs();
    updateMasterButtonLED();
}

/* ============ Helpers ============ */

function noteToName(n) {
    if (n <= 0) return "---";
    const names = ["C","C#","D","D#","E","F","F#","G","G#","A","A#","B"];
    return names[n % 12] + (Math.floor(n/12) - 1);
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
    const stepNotes = getCurrentPattern(trackIdx).steps[stepIdx];
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
    /* Clear all notes from a step */
    getCurrentPattern(trackIdx).steps[stepIdx] = [];
    host_module_set_param(`track_${trackIdx}_step_${stepIdx}_clear`, "1");
}

function setStepNote(trackIdx, stepIdx, note) {
    /* Set a single note (clears others) - for backward compat */
    if (note === 0) {
        clearStep(trackIdx, stepIdx);
    } else {
        getCurrentPattern(trackIdx).steps[stepIdx] = [note];
        host_module_set_param(`track_${trackIdx}_step_${stepIdx}_note`, String(note));
    }
}

/* ============ Lifecycle ============ */

globalThis.init = function() {
    console.log("Multi-track sequencer starting...");
    clearAllLEDs();

    /* Initialize track data with patterns */
    for (let t = 0; t < NUM_TRACKS; t++) {
        tracks[t] = {
            patterns: [],
            currentPattern: 0,
            muted: false,
            channel: t
        };
        /* Initialize 8 patterns per track */
        for (let p = 0; p < NUM_PATTERNS; p++) {
            tracks[t].patterns.push({
                steps: [],
                loopStart: 0,
                loopEnd: NUM_STEPS - 1
            });
            for (let s = 0; s < NUM_STEPS; s++) {
                tracks[t].patterns[p].steps.push([]);
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
                    const stepNotes = getCurrentPattern(currentTrack).steps[currentPlayStep];
                    const trackColor = TRACK_COLORS[currentTrack];

                    for (const note of stepNotes) {
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
    if (isCapacitiveTouchMessage(data)) return;

    const isNote = data[0] === MidiNoteOn || data[0] === MidiNoteOff;
    const isNoteOn = data[0] === MidiNoteOn;
    const isCC = data[0] === MidiCC;
    const note = data[1];
    const velocity = data[2];

    /* Shift button (CC 49) */
    if (isCC && note === MoveShift) {
        shiftHeld = velocity > 0;
        updateTrackLEDs();  // Update to show tracks 5-8 when shift held
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
            }
            updateStepLEDs();
            return;
        }

        if (isNoteOn && velocity > 0) {
            /* Step pressed - record time and show on pads */
            setLED(note, TRACK_COLORS[currentTrack]);
            stepPressTimes[stepIdx] = Date.now();
            stepPadPressed[stepIdx] = false;
            heldStep = stepIdx;
            updatePadLEDs();  /* Show notes in this step on pads */
        } else {
            /* Step released - check per-step tracking */
            const pressTime = stepPressTimes[stepIdx];
            const padPressed = stepPadPressed[stepIdx];

            if (pressTime !== undefined && !padPressed) {
                const holdDuration = Date.now() - pressTime;
                if (holdDuration < HOLD_THRESHOLD_MS) {
                    /* Quick tap - clear if has data, add note if empty */
                    const stepNotes = getCurrentPattern(currentTrack).steps[stepIdx];
                    if (stepNotes.length > 0) {
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
                const stepNotes = getCurrentPattern(currentTrack).steps[heldStep];
                displayMessage(
                    undefined,
                    `Step ${heldStep + 1}`,
                    `Notes: ${notesToString(stepNotes)}`,
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
                    const stepNotes = getCurrentPattern(currentTrack).steps[currentPlayStep];
                    if (!stepNotes.includes(midiNote) && stepNotes.length < 4) {
                        stepNotes.push(midiNote);
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

    /* Knobs (CC 71-78) - in master mode, set MIDI channel for each track */
    const knobs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];
    if (isCC && knobs.includes(note)) {
        const knobIdx = knobs.indexOf(note);
        /* Knob 1 (leftmost) = track 1, knob 8 (rightmost) = track 8 */
        const trackIdx = knobIdx;

        if (masterMode) {
            /* Change MIDI channel for this track */
            /* Knob values: 1-63 = increment, 65-127 = decrement */
            let channel = tracks[trackIdx].channel;
            if (velocity >= 1 && velocity <= 63) {
                channel = (channel + 1) % 16;
            } else if (velocity >= 65 && velocity <= 127) {
                channel = (channel - 1 + 16) % 16;
            }
            tracks[trackIdx].channel = channel;
            host_module_set_param(`track_${trackIdx}_channel`, String(channel));
            /* Update the full master display to show new channel */
            updateDisplay();
        }
        return;
    }
};

globalThis.onMidiMessageExternal = function(data) {};
