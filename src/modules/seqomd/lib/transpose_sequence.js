/*
 * Transpose Sequence
 * Manages the global transpose sequence (max 16 steps with variable duration)
 */

import { state } from './state.js';

const MAX_TRANSPOSE_STEPS = 16;
const DEFAULT_DURATION = 4; /* 1 bar = 4 beats */
const MIN_DURATION = 1;     /* 1 beat */
const MAX_DURATION = 64;    /* 16 bars */
const BEAT_GRANULARITY_MAX = 20; /* 5 bars - beat granularity up to here, then bar granularity */
const MIN_TRANSPOSE = -24;  /* 2 octaves down */
const MAX_TRANSPOSE = 24;   /* 2 octaves up */

/**
 * Create a new transpose step
 * @param {number} transpose - Semitone offset (-24 to +24)
 * @param {number} duration - Duration in beats (1-64)
 * @returns {Object}
 */
export function createTransposeStep(transpose = 0, duration = DEFAULT_DURATION) {
    return {
        transpose: Math.max(MIN_TRANSPOSE, Math.min(MAX_TRANSPOSE, transpose)),
        duration: Math.max(MIN_DURATION, Math.min(MAX_DURATION, duration)),
        jump: -1,      /* No jump by default */
        condition: 0   /* No condition by default */
    };
}

/**
 * Set or create a transpose step at the given index
 * @param {number} index - Step index (0-15)
 * @param {number} transpose - Semitone offset
 * @param {number} duration - Duration in beats (optional, keeps existing if updating)
 * @returns {boolean} Success
 */
export function setTransposeStep(index, transpose, duration = null) {
    if (index < 0 || index >= MAX_TRANSPOSE_STEPS) return false;

    /* Ensure array is big enough */
    while (state.transposeSequence.length <= index) {
        state.transposeSequence.push(null);
    }

    const existing = state.transposeSequence[index];
    if (existing) {
        existing.transpose = Math.max(MIN_TRANSPOSE, Math.min(MAX_TRANSPOSE, transpose));
        if (duration !== null) {
            existing.duration = Math.max(MIN_DURATION, Math.min(MAX_DURATION, duration));
        }
    } else {
        state.transposeSequence[index] = createTransposeStep(
            transpose,
            duration !== null ? duration : DEFAULT_DURATION
        );
    }

    return true;
}

/**
 * Update the duration of a transpose step
 * @param {number} index - Step index
 * @param {number} duration - New duration in beats
 * @returns {boolean} Success
 */
export function setTransposeDuration(index, duration) {
    if (index < 0 || index >= state.transposeSequence.length) return false;
    if (!state.transposeSequence[index]) return false;

    state.transposeSequence[index].duration = Math.max(MIN_DURATION, Math.min(MAX_DURATION, duration));
    return true;
}

/**
 * Adjust the duration of a transpose step with smart increments
 * - 1 beat to 5 bars (20 beats): increment by 1 beat
 * - 5 bars to 16 bars: increment by 1 bar (4 beats)
 * @param {number} index - Step index
 * @param {number} delta - Direction (+1 or -1)
 * @returns {number|null} New duration or null if failed
 */
export function adjustTransposeDuration(index, delta) {
    if (index < 0 || index >= state.transposeSequence.length) return null;
    if (!state.transposeSequence[index]) return null;

    const step = state.transposeSequence[index];
    let newDuration = step.duration;

    if (delta > 0) {
        /* Increase */
        if (newDuration < BEAT_GRANULARITY_MAX) {
            /* Beat granularity: 1 beat → 5 bars */
            newDuration += 1;
        } else {
            /* Bar granularity: 5 bars → 6 bars → ... → 16 bars */
            newDuration += 4;
        }
    } else if (delta < 0) {
        /* Decrease */
        if (newDuration <= BEAT_GRANULARITY_MAX) {
            /* Beat granularity */
            newDuration -= 1;
        } else {
            /* Bar granularity - but snap to 5 bars if going below 6 bars */
            newDuration -= 4;
            if (newDuration < BEAT_GRANULARITY_MAX) {
                newDuration = BEAT_GRANULARITY_MAX;
            }
        }
    }

    /* Clamp to valid range */
    step.duration = Math.max(MIN_DURATION, Math.min(MAX_DURATION, newDuration));
    return step.duration;
}

/**
 * Remove a transpose step and shift subsequent steps back
 * @param {number} index - Step index to remove
 * @returns {boolean} Success
 */
export function removeTransposeStep(index) {
    if (index < 0 || index >= state.transposeSequence.length) return false;
    if (!state.transposeSequence[index]) return false;

    state.transposeSequence.splice(index, 1);

    /* Remove trailing nulls */
    while (state.transposeSequence.length > 0 &&
           state.transposeSequence[state.transposeSequence.length - 1] === null) {
        state.transposeSequence.pop();
    }

    return true;
}

/**
 * Get the total duration of the transpose sequence in beats
 * @returns {number}
 */
export function getTotalDuration() {
    let total = 0;
    for (const step of state.transposeSequence) {
        if (step) {
            total += step.duration;
        }
    }
    return total;
}

/**
 * Get the transpose value at a given beat position (handles looping)
 * @param {number} beat - Current beat position
 * @returns {number} Transpose in semitones (0 if empty sequence)
 */
export function getTransposeAtBeat(beat) {
    const totalDuration = getTotalDuration();
    if (totalDuration === 0) return 0;

    /* Handle looping */
    const loopedBeat = beat % totalDuration;

    /* Find which step we're in */
    let accumulated = 0;
    for (const step of state.transposeSequence) {
        if (!step) continue;
        if (loopedBeat < accumulated + step.duration) {
            return step.transpose;
        }
        accumulated += step.duration;
    }

    /* Fallback (shouldn't happen) */
    return 0;
}

/**
 * Get the index of the currently playing transpose step
 * @param {number} beat - Current beat position
 * @returns {number} Step index or -1 if empty
 */
export function getCurrentStepIndex(beat) {
    const totalDuration = getTotalDuration();
    if (totalDuration === 0) return -1;

    const loopedBeat = beat % totalDuration;

    let accumulated = 0;
    let index = 0;
    for (const step of state.transposeSequence) {
        if (!step) {
            index++;
            continue;
        }
        if (loopedBeat < accumulated + step.duration) {
            return index;
        }
        accumulated += step.duration;
        index++;
    }

    return -1;
}

/**
 * Get the number of defined steps (non-null)
 * @returns {number}
 */
export function getStepCount() {
    let count = 0;
    for (const step of state.transposeSequence) {
        if (step) count++;
    }
    return count;
}

/**
 * Get a transpose step by index
 * @param {number} index
 * @returns {Object|null}
 */
export function getTransposeStep(index) {
    if (index < 0 || index >= state.transposeSequence.length) return null;
    return state.transposeSequence[index];
}

/**
 * Clear all transpose steps
 */
export function clearTransposeSequence() {
    state.transposeSequence.length = 0;
}

/**
 * Clone the transpose sequence (for saving)
 * @param {Array} seq - Source sequence
 * @returns {Array}
 */
export function cloneTransposeSequence(seq) {
    return seq.map(step => step ? { ...step } : null).filter(s => s !== null);
}

/**
 * Format duration for display
 * @param {number} beats - Duration in beats
 * @returns {string}
 */
export function formatDuration(beats) {
    if (beats < 4) {
        return `${beats} beat${beats !== 1 ? 's' : ''}`;
    } else if (beats % 4 === 0) {
        const bars = beats / 4;
        return `${bars} bar${bars !== 1 ? 's' : ''}`;
    } else {
        const bars = Math.floor(beats / 4);
        const remainingBeats = beats % 4;
        return `${bars} bar${bars !== 1 ? 's' : ''} ${remainingBeats} beat${remainingBeats !== 1 ? 's' : ''}`;
    }
}

export {
    MAX_TRANSPOSE_STEPS,
    DEFAULT_DURATION,
    MIN_DURATION,
    MAX_DURATION,
    MIN_TRANSPOSE,
    MAX_TRANSPOSE
};
