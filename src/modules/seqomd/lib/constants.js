/*
 * SEQOMD Constants
 * All configuration values and lookup tables
 */

import {
    Black, White, LightGrey, DarkGrey, Navy, BrightGreen, Cyan, BrightRed,
    OrangeRed, VividYellow, RoyalBlue, Purple,
    NeonPink, BurntOrange, Lime, TealGreen, AzureBlue, Violet, HotMagenta,
    Rose, Mustard, OliveGreen, MutedTeal, MutedBlue, MutedViolet, DeepMagenta,
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    BlueViolet, SkyBlue, DeepBlue, DarkPurple, DarkViolet, LightYellow, Ochre,
    VeryDarkRed, VeryDarkYellow, VeryDarkGreen, DarkOliveGreen, DeepBrownYellow,
    DeepTeal, DarkBlue, DeepBlueIndigo, DeepViolet, DeepPlum, ShadowMauve
} from "./shared-constants.js";

/* ============ Core Constants ============ */

export const NUM_TRACKS = 16;
export const NUM_STEPS = 64;          // Max steps per pattern (was 16)
export const STEPS_PER_PAGE = 16;     // Steps shown on hardware at once
export const MAX_PAGES = 4;           // 64 steps / 16 per page
export const DEFAULT_TRACK_LENGTH = 16;  // Default track length
export const NUM_PATTERNS = 16;
export const NUM_SETS = 32;

/* ============ Reset Constants ============ */

export const RESET_INF = 0;           // 0 = infinite (never reset)
export const MAX_RESET = 256;         // Maximum reset length in steps

/* ============ Gate Constants ============ */

export const DEFAULT_GATE = 95;       // Default gate percentage (1-100)
export const MIN_GATE = 1;
export const MAX_GATE = 100;

/* ============ Timing ============ */

export const HOLD_THRESHOLD_MS = 300;  // Time threshold for hold vs tap
export const COPY_HOLD_MS = 500;       // Time threshold for copy operation
export const DISPLAY_RETURN_MS = 2000; // Time before display returns to default after parameter change

/* ============ Persistence ============ */

export const DATA_DIR = '/data/UserData/move-anything-data/seqomd';
export const SETS_DIR = DATA_DIR + '/sets';
export const SETS_FILE = DATA_DIR + '/sets.json';  // Legacy, for migration

/* ============ MIDI ============ */

export const MASTER_CC_CHANNEL = 15;   // Channel 16 (0-indexed) for pattern mode CCs

/* ============ Knob LEDs ============ */

export const MoveKnobLEDs = [MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4, MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8];

/* ============ Track Colors ============ */

export const TRACK_COLORS = [
    BrightRed,    // Track 1
    OrangeRed,    // Track 2
    VividYellow,  // Track 3
    BrightGreen,  // Track 4
    Lime,         // Track 5 - warm green
    BurntOrange,  // Track 6 - warm orange/brown
    Rose,         // Track 7 - warm pink
    LightYellow,  // Track 8 - warm yellow
    Cyan,         // Track 9 - cold colors start
    AzureBlue,    // Track 10
    RoyalBlue,    // Track 11
    Navy,         // Track 12
    BlueViolet,   // Track 13
    Violet,       // Track 14
    Purple,       // Track 15
    LightGrey     // Track 16
];

export const TRACK_NAMES = [
    "1", "2", "3", "4", "5", "6", "7", "8",
    "9", "10", "11", "12", "13", "14", "15", "16"
];

/* Dim versions of track colors for non-selected tracks with content */
export const TRACK_COLORS_DIM = [
    65,           // Deep Red (dim red)
    67,           // Brick (dim orange)
    73,           // Dull Yellow
    79,           // Dull Green
    OliveGreen,   // Track 5 - dim lime
    Mustard,      // Track 6 - dim burnt orange
    Rose,         // Track 7 - dim rose (already muted)
    Ochre,        // Track 8 - dim light yellow
    MutedBlue,    // Track 9 - dim cold colors
    SkyBlue,      // Track 10
    Navy,         // Track 11
    DeepBlue,     // Track 12
    DarkPurple,   // Track 13
    MutedViolet,  // Track 14
    DarkViolet,   // Track 15
    DarkGrey      // Track 16
];

/* Very dim versions of track colors for CC-only steps (no notes) */
export const TRACK_COLORS_VERY_DIM = [
    VeryDarkRed,      // Track 1
    VeryDarkRed,      // Track 2 - similar to track 1
    VeryDarkYellow,   // Track 3
    VeryDarkGreen,    // Track 4
    DarkOliveGreen,   // Track 5
    DeepBrownYellow,  // Track 6
    ShadowMauve,      // Track 7
    VeryDarkYellow,   // Track 8
    DeepTeal,         // Track 9
    DarkBlue,         // Track 10
    DarkBlue,         // Track 11
    DarkBlue,         // Track 12
    DeepBlueIndigo,   // Track 13
    DeepViolet,       // Track 14
    DeepPlum,         // Track 15
    DarkGrey          // Track 16
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

/*
 * Ratchet encoding scheme for three modes:
 * - Values 1-8: Regular ratchet (1x-8x)
 * - Values 10-16: Velocity Ramp Up (2x-8x) - count = value - 8
 * - Values 20-26: Velocity Ramp Down (2x-8x) - count = value - 18
 *
 * DSP decoding:
 *   if (val >= 20) → mode=RAMP_DOWN, count = val - 18
 *   else if (val >= 10) → mode=RAMP_UP, count = val - 8
 *   else → mode=REGULAR, count = val
 */
export const RATCHET_VALUES = [
    1, 2, 3, 4, 5, 6, 7, 8,        // Regular: 1x-8x (indices 0-7)
    10, 11, 12, 13, 14, 15, 16,    // Ramp Up: 2x-8x (indices 8-14)
    20, 21, 22, 23, 24, 25, 26     // Ramp Down: 2x-8x (indices 15-21)
];

/* Helper functions to decode ratchet values */
export function getRatchetMode(value) {
    if (value >= 20) return 'ramp_down';
    if (value >= 10) return 'ramp_up';
    return 'regular';
}

export function getRatchetCount(value) {
    if (value >= 20) return value - 18;
    if (value >= 10) return value - 8;
    return value;
}

export function getRatchetDisplayName(value) {
    const count = getRatchetCount(value);
    const mode = getRatchetMode(value);

    if (mode === 'ramp_up') return `Ramp Up: ${count}x`;
    if (mode === 'ramp_down') return `Ramp Dn: ${count}x`;
    return `Ratchet: ${count}x`;
}

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

/* ============ Arpeggiator Options ============ */

export const ARP_MODES = [
    { name: 'Off' },
    { name: 'Up' },
    { name: 'Down' },
    { name: 'Up-Down' },
    { name: 'Down-Up' },
    { name: 'Up & Down' },
    { name: 'Down & Up' },
    { name: 'Random' },
    { name: 'Chord' },
    { name: 'Outside-In' },
    { name: 'Inside-Out' },
    { name: 'Converge' },
    { name: 'Diverge' },
    { name: 'Thumb' },
    { name: 'Pinky' }
];

/* ARP_SPEEDS: Musical note values (16 steps = 1 bar)
 * Must match DSP ARP_STEP_RATES[] */
export const ARP_SPEEDS = [
    { name: '1/32' },   // 32nd notes (2 per step)
    { name: '1/24' },   // triplet 16ths
    { name: '1/16' },   // 16th notes (1 per step)
    { name: '1/12' },   // triplet 8ths
    { name: '1/8' },    // 8th notes
    { name: '1/6' },    // triplet quarters
    { name: '1/4' },    // quarter notes
    { name: '1/3' },    // triplet halves
    { name: '1/2' },    // half notes
    { name: '1/1' }     // whole note
];

export const ARP_OCTAVES = [
    { name: '0' },
    { name: '+1' },
    { name: '+2' },
    { name: '-1' },
    { name: '-2' },
    { name: '±1' },
    { name: '±2' }
];

export const ARP_LAYERS = [
    { name: 'Layer' },    // 0: Arps play over each other (default)
    { name: 'Cut' }       // 1: New step kills previous arp notes
];

export const DEFAULT_ARP_SPEED = 2;  /* 1/16 = 1 note per step */

/* ============ Arp Play Steps Helpers ============ */

/**
 * Get the pattern length from play steps value (highest set bit position + 1)
 * @param {number} playSteps - Value 1-255
 * @returns {number} Pattern length in bits (1-8)
 */
export function getPlayStepsLength(playSteps) {
    if (playSteps <= 0) return 1;
    return Math.floor(Math.log2(playSteps)) + 1;
}

/**
 * Get the rotated binary string for display
 * Shows the pattern as it will be played from the start position
 * @param {number} playSteps - Value 1-255
 * @param {number} startPos - Start position 0-7
 * @returns {string} Binary string (e.g., "101")
 */
export function getRotatedPlaySteps(playSteps, startPos) {
    if (playSteps <= 0) return "1";
    const len = getPlayStepsLength(playSteps);
    let result = '';
    for (let i = 0; i < len; i++) {
        const bitIndex = (startPos + i) % len;
        result += (playSteps >> bitIndex) & 1;
    }
    return result;
}

/* ============ Pattern Generator ============ */

export const GENERATE_STYLES = [
    { name: 'Random', short: 'RND' },
    { name: 'Euclidean', short: 'EUC' },
    { name: 'Rising', short: 'RIS' },
    { name: 'Falling', short: 'FAL' },
    { name: 'Arc', short: 'ARC' },
    { name: 'Pulse', short: 'PLS' },
    { name: 'Offbeat', short: 'OFF' },
    { name: 'Clustered', short: 'CLU' }
];

export const GENERATOR_SCALES = [
    { name: 'Detected', short: 'Det', notes: null },
    { name: 'Chromatic', short: 'Chr', notes: [0,1,2,3,4,5,6,7,8,9,10,11] },
    { name: 'Minor Penta', short: 'mPn', notes: [0, 3, 5, 7, 10] },
    { name: 'Major Penta', short: 'MPn', notes: [0, 2, 4, 7, 9] },
    { name: 'Blues', short: 'Blu', notes: [0, 3, 5, 6, 7, 10] },
    { name: 'Major', short: 'Maj', notes: [0, 2, 4, 5, 7, 9, 11] },
    { name: 'Minor', short: 'Min', notes: [0, 2, 3, 5, 7, 8, 10] },
    { name: 'Dorian', short: 'Dor', notes: [0, 2, 3, 5, 7, 9, 10] },
    { name: 'Mixolydian', short: 'Mix', notes: [0, 2, 4, 5, 7, 9, 10] },
    { name: 'Phrygian', short: 'Phr', notes: [0, 1, 3, 5, 7, 8, 10] },
    { name: 'Lydian', short: 'Lyd', notes: [0, 2, 4, 6, 7, 9, 11] },
    { name: 'Locrian', short: 'Loc', notes: [0, 1, 3, 5, 6, 8, 10] },
    { name: 'Harm Minor', short: 'HMi', notes: [0, 2, 3, 5, 7, 8, 11] },
    { name: 'Mel Minor', short: 'MMi', notes: [0, 2, 3, 5, 7, 9, 11] },
    { name: 'Whole Tone', short: 'Whl', notes: [0, 2, 4, 6, 8, 10] },
    { name: 'Dim HW', short: 'DHW', notes: [0, 1, 3, 4, 6, 7, 9, 10] },
    { name: 'Dim WH', short: 'DWH', notes: [0, 2, 3, 5, 6, 8, 9, 11] }
];

export const GENERATOR_ROOTS = [
    { name: 'Auto', short: 'Aut' },
    { name: 'C', short: 'C' },
    { name: 'C#', short: 'C#' },
    { name: 'D', short: 'D' },
    { name: 'D#', short: 'D#' },
    { name: 'E', short: 'E' },
    { name: 'F', short: 'F' },
    { name: 'F#', short: 'F#' },
    { name: 'G', short: 'G' },
    { name: 'G#', short: 'G#' },
    { name: 'A', short: 'A' },
    { name: 'A#', short: 'A#' },
    { name: 'B', short: 'B' }
];

/* Default generator parameters */
export const DEFAULT_GENERATE_PARAMS = {
    length: 16,
    density: 50,
    voices: 1,
    scale: 0,      // 0 = Detected
    root: 0,       // 0 = Auto
    octave: 3,
    range: 2,
    variation: 64
};
