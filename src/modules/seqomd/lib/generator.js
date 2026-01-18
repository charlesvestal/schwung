/*
 * Pattern Generator
 * Algorithmic pattern generation for SEQOMD
 */

import { GENERATE_STYLES, GENERATOR_SCALES, NUM_STEPS } from './constants.js';
import { createEmptyStep } from './data.js';
import { detectScale } from './scale_detection.js';

/* ============ Euclidean Algorithm ============ */

/**
 * Bjorklund's algorithm for Euclidean rhythms
 * Distributes k pulses as evenly as possible over n steps
 * @param {number} pulses - Number of hits (k)
 * @param {number} steps - Pattern length (n)
 * @returns {number[]} Array of 0s and 1s
 */
function euclidean(pulses, steps) {
    if (pulses >= steps) {
        return Array(steps).fill(1);
    }
    if (pulses <= 0) {
        return Array(steps).fill(0);
    }

    let pattern = [];
    let counts = [];
    let remainders = [];

    let divisor = steps - pulses;
    remainders.push(pulses);
    let level = 0;

    while (remainders[level] > 1) {
        counts.push(Math.floor(divisor / remainders[level]));
        remainders.push(divisor % remainders[level]);
        divisor = remainders[level];
        level++;
    }
    counts.push(divisor);

    function build(lvl) {
        if (lvl === -1) {
            pattern.push(0);
        } else if (lvl === -2) {
            pattern.push(1);
        } else {
            for (let i = 0; i < counts[lvl]; i++) {
                build(lvl - 1);
            }
            if (remainders[lvl] !== 0) {
                build(lvl - 2);
            }
        }
    }

    build(level);
    return pattern;
}

/**
 * Rotate a pattern array by offset positions
 * @param {number[]} pattern - Array to rotate
 * @param {number} offset - Positions to rotate (positive = right)
 * @returns {number[]} Rotated array
 */
function rotatePattern(pattern, offset) {
    if (offset === 0 || pattern.length === 0) return pattern;
    const normalizedOffset = ((offset % pattern.length) + pattern.length) % pattern.length;
    return [...pattern.slice(-normalizedOffset), ...pattern.slice(0, -normalizedOffset)];
}

/* ============ Step Selection Algorithms ============ */

/**
 * Calculate which steps should have notes based on style
 * @param {number} length - Pattern length
 * @param {number} density - Percentage of steps with notes (5-100)
 * @param {number} styleIdx - Index into GENERATE_STYLES
 * @param {number} variation - Style-specific variation (0-127)
 * @returns {number[]} Array of step indices that should have notes
 */
function calculateActiveSteps(length, density, styleIdx, variation) {
    const numNotes = Math.max(1, Math.round((density / 100) * length));
    const style = GENERATE_STYLES[styleIdx].name;

    switch (style) {
        case 'Euclidean': {
            const pattern = euclidean(numNotes, length);
            const rotation = Math.floor((variation / 127) * length);
            const rotated = rotatePattern(pattern, rotation);
            return rotated.map((v, i) => v === 1 ? i : -1).filter(i => i >= 0);
        }

        case 'Pulse': {
            // Strong beats first: 0, 4, 8, 12, then 2, 6, 10, 14, then fill
            const steps = [];
            const priorities = [];
            for (let i = 0; i < length; i++) {
                if (i % 4 === 0) priorities.push({ step: i, priority: 0 });
                else if (i % 2 === 0) priorities.push({ step: i, priority: 1 });
                else priorities.push({ step: i, priority: 2 });
            }
            priorities.sort((a, b) => a.priority - b.priority || a.step - b.step);
            for (let i = 0; i < numNotes && i < priorities.length; i++) {
                steps.push(priorities[i].step);
            }
            return steps.sort((a, b) => a - b);
        }

        case 'Offbeat': {
            // Offbeats first: 1, 3, 5, 7, etc., then even beats
            const steps = [];
            const priorities = [];
            for (let i = 0; i < length; i++) {
                if (i % 2 === 1) priorities.push({ step: i, priority: 0 });
                else if (i % 4 === 2) priorities.push({ step: i, priority: 1 });
                else priorities.push({ step: i, priority: 2 });
            }
            priorities.sort((a, b) => a.priority - b.priority || a.step - b.step);
            for (let i = 0; i < numNotes && i < priorities.length; i++) {
                steps.push(priorities[i].step);
            }
            return steps.sort((a, b) => a - b);
        }

        case 'Clustered': {
            // Groups of notes with gaps
            const steps = [];
            const minCluster = 2;
            const maxCluster = 2 + Math.floor((variation / 127) * 6);
            let remaining = numNotes;
            let position = 0;

            while (remaining > 0 && position < length) {
                const clusterSize = Math.min(
                    remaining,
                    minCluster + Math.floor(Math.random() * (maxCluster - minCluster + 1))
                );
                for (let i = 0; i < clusterSize && position < length; i++) {
                    steps.push(position++);
                    remaining--;
                }
                const gap = 2 + Math.floor(Math.random() * 5);
                position += gap;
            }
            return steps;
        }

        case 'Random':
        case 'Rising':
        case 'Falling':
        case 'Arc':
        default: {
            // Random distribution
            const allSteps = Array.from({ length }, (_, i) => i);
            // Shuffle
            for (let i = allSteps.length - 1; i > 0; i--) {
                const j = Math.floor(Math.random() * (i + 1));
                [allSteps[i], allSteps[j]] = [allSteps[j], allSteps[i]];
            }
            return allSteps.slice(0, numNotes).sort((a, b) => a - b);
        }
    }
}

/* ============ Note Generation ============ */

/**
 * Get valid MIDI notes within range that are in the scale
 * @param {number[]} scaleNotes - Scale intervals (0-11)
 * @param {number} root - Root note (0-11)
 * @param {number} baseOctave - Base octave (1-6)
 * @param {number} octaveRange - Number of octaves (1-4)
 * @returns {number[]} Array of valid MIDI notes
 */
function getValidNotes(scaleNotes, root, baseOctave, octaveRange) {
    const lowMidi = (baseOctave + 1) * 12 + root;
    const highMidi = lowMidi + (octaveRange * 12) - 1;
    const validNotes = [];

    for (let midi = lowMidi; midi <= highMidi && midi <= 127; midi++) {
        const pitchClass = ((midi - root) % 12 + 12) % 12;
        if (scaleNotes.includes(pitchClass)) {
            validNotes.push(midi);
        }
    }

    return validNotes;
}

/**
 * Select notes near a center index with some spread
 * @param {number[]} validNotes - Available notes
 * @param {number} centerIdx - Center index in validNotes
 * @param {number} count - Number of notes to select
 * @returns {number[]} Selected MIDI notes
 */
function selectNotesNear(validNotes, centerIdx, count) {
    if (validNotes.length === 0) return [];

    const notes = [];
    const used = new Set();
    centerIdx = Math.max(0, Math.min(validNotes.length - 1, centerIdx));

    for (let i = 0; i < count && notes.length < validNotes.length; i++) {
        // Spread notes around center with some randomness
        let idx = centerIdx + Math.floor((Math.random() - 0.5) * 8);
        idx = Math.max(0, Math.min(validNotes.length - 1, idx));

        // Find nearest unused note
        for (let offset = 0; offset < validNotes.length; offset++) {
            const tryUp = idx + offset;
            const tryDown = idx - offset;

            if (tryUp < validNotes.length && !used.has(tryUp)) {
                notes.push(validNotes[tryUp]);
                used.add(tryUp);
                break;
            }
            if (tryDown >= 0 && !used.has(tryDown)) {
                notes.push(validNotes[tryDown]);
                used.add(tryDown);
                break;
            }
        }
    }

    return notes.sort((a, b) => a - b);
}

/**
 * Generate notes for a step
 * @param {number} stepIdx - Step index
 * @param {number} length - Total pattern length
 * @param {number} voices - Number of notes per step
 * @param {number[]} validNotes - Available notes in scale/range
 * @param {number} styleIdx - Style index
 * @param {number} variation - Variation value
 * @returns {number[]} MIDI notes for this step
 */
function generateNotesForStep(stepIdx, length, voices, validNotes, styleIdx, variation) {
    if (validNotes.length === 0) return [60]; // Fallback to middle C

    const style = GENERATE_STYLES[styleIdx].name;
    const position = stepIdx / Math.max(1, length - 1); // 0.0 to 1.0

    switch (style) {
        case 'Rising': {
            const steepness = variation / 127;
            const curved = Math.pow(position, 1 + steepness * 2);
            const centerIdx = Math.floor(curved * (validNotes.length - 1));
            return selectNotesNear(validNotes, centerIdx, voices);
        }

        case 'Falling': {
            const steepness = variation / 127;
            const curved = Math.pow(1 - position, 1 + steepness * 2);
            const centerIdx = Math.floor(curved * (validNotes.length - 1));
            return selectNotesNear(validNotes, centerIdx, voices);
        }

        case 'Arc': {
            const peakPosition = variation / 127; // Where the peak is (0=start, 1=end)
            let arcValue;
            if (position <= peakPosition) {
                arcValue = peakPosition > 0 ? position / peakPosition : 0;
            } else {
                arcValue = peakPosition < 1 ? 1 - (position - peakPosition) / (1 - peakPosition) : 1;
            }
            const centerIdx = Math.floor(arcValue * (validNotes.length - 1));
            return selectNotesNear(validNotes, centerIdx, voices);
        }

        case 'Pulse': {
            // Root-heavy on strong beats
            const isDownbeat = stepIdx % 4 === 0;
            if (isDownbeat && voices >= 1) {
                // Start from root
                const rootIdx = 0;
                return selectNotesNear(validNotes, rootIdx, voices);
            }
            // Random for other beats
            const centerIdx = Math.floor(Math.random() * validNotes.length);
            return selectNotesNear(validNotes, centerIdx, voices);
        }

        default: {
            // Random selection
            const centerIdx = Math.floor(Math.random() * validNotes.length);
            return selectNotesNear(validNotes, centerIdx, voices);
        }
    }
}

/* ============ Velocity Generation ============ */

/**
 * Generate velocities for notes
 * @param {number} noteCount - Number of notes
 * @param {number} stepIdx - Step index
 * @param {number} length - Pattern length
 * @param {number} styleIdx - Style index
 * @param {number} variation - Variation value
 * @returns {number[]} Velocities for each note
 */
function generateVelocities(noteCount, stepIdx, length, styleIdx, variation) {
    const style = GENERATE_STYLES[styleIdx].name;
    const baseVelocity = 90;
    const velocities = [];

    for (let i = 0; i < noteCount; i++) {
        let vel = baseVelocity;

        switch (style) {
            case 'Pulse': {
                const isDownbeat = stepIdx % 4 === 0;
                const accentAmount = (variation / 127) * 30;
                vel = isDownbeat ? baseVelocity + accentAmount : baseVelocity - accentAmount / 2;
                break;
            }

            case 'Rising': {
                const position = stepIdx / Math.max(1, length - 1);
                vel = 60 + position * 60;
                break;
            }

            case 'Falling': {
                const position = stepIdx / Math.max(1, length - 1);
                vel = 120 - position * 60;
                break;
            }

            default: {
                // Random variation
                vel = baseVelocity + (Math.random() - 0.5) * 25;
            }
        }

        // Add slight humanization
        vel += (Math.random() - 0.5) * 10;

        // Clamp to valid MIDI range
        velocities.push(Math.max(1, Math.min(127, Math.round(vel))));
    }

    return velocities;
}

/* ============ Main Generator Function ============ */

/**
 * Generate a pattern based on parameters
 * @param {object} params - Generator parameters
 * @param {number} params.length - Pattern length (1-64)
 * @param {number} params.density - Note density (5-100%)
 * @param {number} params.voices - Notes per step (1-7)
 * @param {number} params.scale - Scale index
 * @param {number} params.root - Root index (0=auto, 1-12=C-B)
 * @param {number} params.octave - Base octave (1-6)
 * @param {number} params.range - Octave range (1-4)
 * @param {number} params.variation - Style-specific variation (0-127)
 * @param {number} styleIdx - Style index
 * @param {object} detectedScale - Result from detectScale() or null
 * @returns {object[]} Array of step objects
 */
export function generatePattern(params, styleIdx, detectedScale) {
    const { length, density, voices, scale, root, octave, range, variation } = params;

    // Determine scale notes
    let scaleNotes;
    if (scale === 0 && detectedScale && detectedScale.scaleNotes) {
        // Use detected scale
        scaleNotes = Array.from(detectedScale.scaleNotes);
    } else if (scale === 0) {
        // No detected scale, fall back to chromatic
        scaleNotes = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
    } else {
        scaleNotes = GENERATOR_SCALES[scale].notes || [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11];
    }

    // Determine root note
    let rootNote;
    if (root === 0 && detectedScale) {
        rootNote = detectedScale.root;
    } else if (root === 0) {
        rootNote = 0; // C
    } else {
        rootNote = root - 1; // 1=C -> 0, 2=C# -> 1, etc.
    }

    // Get valid notes in range
    const validNotes = getValidNotes(scaleNotes, rootNote, octave, range);

    // Calculate which steps get notes
    const activeSteps = calculateActiveSteps(length, density, styleIdx, variation);

    // Create pattern steps
    const steps = [];
    for (let s = 0; s < NUM_STEPS; s++) {
        const step = createEmptyStep();

        if (s < length && activeSteps.includes(s)) {
            step.notes = generateNotesForStep(s, length, voices, validNotes, styleIdx, variation);
            step.velocities = generateVelocities(step.notes.length, s, length, styleIdx, variation);
        }

        steps.push(step);
    }

    return steps;
}

/**
 * Check if a pattern has any content
 * @param {object} pattern - Pattern object
 * @returns {boolean} True if pattern has notes or CCs
 */
export function patternHasContent(pattern) {
    for (const step of pattern.steps) {
        if (step.notes.length > 0 || step.cc1 >= 0 || step.cc2 >= 0) {
            return true;
        }
    }
    return false;
}
