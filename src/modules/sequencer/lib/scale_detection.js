/*
 * Scale Detection
 * Analyzes notes from chordFollow tracks to detect the most likely scale
 */

/* Scale templates ordered by preference (simpler scales first) */
const SCALES = [
    /* Pentatonic (5 notes) - most preferred */
    { name: 'Minor Penta', notes: [0, 3, 5, 7, 10] },
    { name: 'Major Penta', notes: [0, 2, 4, 7, 9] },

    /* Blues (6 notes) */
    { name: 'Blues', notes: [0, 3, 5, 6, 7, 10] },
    { name: 'Whole Tone', notes: [0, 2, 4, 6, 8, 10] },

    /* Common 7-note scales */
    { name: 'Major', notes: [0, 2, 4, 5, 7, 9, 11] },
    { name: 'Natural Minor', notes: [0, 2, 3, 5, 7, 8, 10] },

    /* Modes (7 notes) */
    { name: 'Dorian', notes: [0, 2, 3, 5, 7, 9, 10] },
    { name: 'Mixolydian', notes: [0, 2, 4, 5, 7, 9, 10] },
    { name: 'Phrygian', notes: [0, 1, 3, 5, 7, 8, 10] },
    { name: 'Lydian', notes: [0, 2, 4, 6, 7, 9, 11] },
    { name: 'Locrian', notes: [0, 1, 3, 5, 6, 8, 10] },

    /* Other 7-note */
    { name: 'Harmonic Minor', notes: [0, 2, 3, 5, 7, 8, 11] },
    { name: 'Melodic Minor', notes: [0, 2, 3, 5, 7, 9, 11] },

    /* 8-note scales - least preferred */
    { name: 'Diminished HW', notes: [0, 1, 3, 4, 6, 7, 9, 10] },
    { name: 'Diminished WH', notes: [0, 2, 3, 5, 6, 8, 9, 11] },
];

/* Note names for display */
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

/**
 * Collect all unique pitch classes from tracks with chordFollow=true
 * Looks at ALL patterns in each track
 * @param {Array} tracks - Array of track data
 * @param {Array} chordFollow - Array of 8 booleans
 * @returns {Set} Set of pitch classes (0-11)
 */
export function collectPitchClasses(tracks, chordFollow) {
    const pitchClasses = new Set();

    for (let t = 0; t < tracks.length; t++) {
        if (!chordFollow[t]) continue;

        const track = tracks[t];
        for (const pattern of track.patterns) {
            for (const step of pattern.steps) {
                for (const note of step.notes) {
                    pitchClasses.add(note % 12);
                }
            }
        }
    }

    return pitchClasses;
}

/**
 * Transpose a scale template to a given root
 * @param {Array} scaleNotes - Scale intervals from root
 * @param {number} root - Root note (0-11)
 * @returns {Set} Set of pitch classes in the scale
 */
function transposeScale(scaleNotes, root) {
    return new Set(scaleNotes.map(n => (n + root) % 12));
}

/**
 * Score how well a set of pitch classes fits a scale
 * @param {Set} pitchClasses - Notes being analyzed
 * @param {Set} scaleNotes - Notes in the scale
 * @param {number} scaleSize - Number of notes in the scale template
 * @returns {number} Score (higher is better)
 */
function scoreScale(pitchClasses, scaleNotes, scaleSize) {
    if (pitchClasses.size === 0) return 0;

    let inScale = 0;
    for (const pc of pitchClasses) {
        if (scaleNotes.has(pc)) {
            inScale++;
        }
    }

    const fitScore = inScale / pitchClasses.size;

    /* Small bonus for simpler scales (fewer notes) */
    const sizeBonus = (1 / scaleSize) * 0.1;

    return fitScore + sizeBonus;
}

/**
 * Detect the most likely scale from the notes in chordFollow tracks
 * @param {Array} tracks - Array of track data
 * @param {Array} chordFollow - Array of 8 booleans
 * @returns {Object|null} { root: 0-11, scaleName: string, scaleNotes: Set } or null if no notes
 */
export function detectScale(tracks, chordFollow) {
    const pitchClasses = collectPitchClasses(tracks, chordFollow);

    if (pitchClasses.size === 0) {
        return null;
    }

    let bestScore = -1;
    let bestResult = null;

    /* Test each root (0-11) x each scale */
    for (let root = 0; root < 12; root++) {
        for (let scaleIdx = 0; scaleIdx < SCALES.length; scaleIdx++) {
            const scale = SCALES[scaleIdx];
            const transposed = transposeScale(scale.notes, root);
            const score = scoreScale(pitchClasses, transposed, scale.notes.length);

            /* Prefer higher score, or earlier scale (simpler) on tie */
            if (score > bestScore) {
                bestScore = score;
                bestResult = {
                    root: root,
                    scaleName: scale.name,
                    scaleNotes: transposed
                };
            }
        }
    }

    return bestResult;
}

/**
 * Get the display name for a detected scale
 * @param {Object} detected - Result from detectScale
 * @returns {string} e.g., "C Minor Penta"
 */
export function getScaleDisplayName(detected) {
    if (!detected) return 'No scale';
    return `${NOTE_NAMES[detected.root]} ${detected.scaleName}`;
}

/**
 * Check if a pitch class is the root of the detected scale
 * @param {number} pitchClass - 0-11
 * @param {Object} detected - Result from detectScale
 * @returns {boolean}
 */
export function isRoot(pitchClass, detected) {
    if (!detected) return false;
    return (pitchClass % 12) === detected.root;
}

/**
 * Check if a pitch class is in the detected scale
 * @param {number} pitchClass - 0-11
 * @param {Object} detected - Result from detectScale
 * @returns {boolean}
 */
export function isInScale(pitchClass, detected) {
    if (!detected) return false;
    return detected.scaleNotes.has(pitchClass % 12);
}

/**
 * Get note name from MIDI note number
 * @param {number} midiNote - MIDI note number
 * @returns {string} e.g., "C4"
 */
export function midiNoteToName(midiNote) {
    const octave = Math.floor(midiNote / 12) - 1;
    const pitchClass = midiNote % 12;
    return `${NOTE_NAMES[pitchClass]}${octave}`;
}

export { NOTE_NAMES, SCALES };
