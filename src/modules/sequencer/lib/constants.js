/*
 * SEQOMD Constants
 * All configuration values and lookup tables
 */

import {
    Black, White, LightGrey, DarkGrey, Navy, BrightGreen, Cyan, BrightRed,
    OrangeRed, VividYellow, RoyalBlue, Purple,
    NeonPink, BurntOrange, Lime, TealGreen, AzureBlue, Violet, HotMagenta,
    Rose, Mustard, OliveGreen, MutedTeal, MutedBlue, MutedViolet, DeepMagenta,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8
} from "../../../shared/constants.mjs";

/* ============ Core Constants ============ */

export const NUM_TRACKS = 16;
export const NUM_STEPS = 16;
export const NUM_PATTERNS = 16;
export const NUM_SETS = 32;

/* ============ Timing ============ */

export const HOLD_THRESHOLD_MS = 300;  // Time threshold for hold vs tap

/* ============ Persistence ============ */

export const DATA_DIR = '/data/UserData/move-anything-data/sequencer';
export const SETS_DIR = DATA_DIR + '/sets';
export const SETS_FILE = DATA_DIR + '/sets.json';  // Legacy, for migration

/* ============ MIDI ============ */

export const MASTER_CC_CHANNEL = 15;   // Channel 16 (0-indexed) for pattern mode CCs

/* ============ Knob LEDs ============ */

export const MoveKnobLEDs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];

/* ============ Track Colors ============ */

export const TRACK_COLORS = [
    BrightRed,    // Track 1 - Kick
    OrangeRed,    // Track 2 - Snare
    VividYellow,  // Track 3 - Perc
    BrightGreen,  // Track 4 - Sample
    Cyan,         // Track 5 - Bass
    RoyalBlue,    // Track 6 - Lead
    Purple,       // Track 7 - Arp
    White,        // Track 8 - Chord
    NeonPink,     // Track 9 - Kick2
    BurntOrange,  // Track 10 - Snare2
    Lime,         // Track 11 - Perc2
    TealGreen,    // Track 12 - Sample2
    AzureBlue,    // Track 13 - Bass2
    Violet,       // Track 14 - Lead2
    HotMagenta,   // Track 15 - Arp2
    LightGrey     // Track 16 - Chord2
];

export const TRACK_NAMES = [
    "Kick", "Snare", "Perc", "Sample",
    "Bass", "Lead", "Arp", "Chord",
    "Kick2", "Snare2", "Perc2", "Sample2",
    "Bass2", "Lead2", "Arp2", "Chord2"
];

/* Dim versions of track colors for non-selected tracks with content */
export const TRACK_COLORS_DIM = [
    65,           // Deep Red (dim red)
    67,           // Brick (dim orange)
    73,           // Dull Yellow
    79,           // Dull Green
    87,           // Dark Teal (dim cyan)
    17,           // Navy (dim blue)
    107,          // Dark Purple
    118,          // Light Grey (dim white)
    Rose,         // Dim pink
    Mustard,      // Dim orange
    OliveGreen,   // Dim lime
    MutedTeal,    // Dim teal
    MutedBlue,    // Dim azure
    MutedViolet,  // Dim violet
    DeepMagenta,  // Dim magenta
    DarkGrey      // Dim grey
];

/* ============ Speed Options ============ */

export const SPEED_OPTIONS = [
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
export const DEFAULT_SPEED_INDEX = 4;  // 1x

/* ============ Ratchet Options ============ */

export const RATCHET_VALUES = [1, 2, 3, 4, 6, 8];

/* ============ Condition Options ============ */

/*
 * n = loop cycle length, m = which iteration to play on
 * Example: 2:3 means "play on the 2nd of every 3 loops"
 */
export const CONDITIONS = [
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
