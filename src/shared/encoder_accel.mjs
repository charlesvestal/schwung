/*
 * Encoder Acceleration
 *
 * Provides velocity-sensitive step sizes for rotary encoders.
 * Slow turns = fine control (step 1), fast turns = coarse control (larger steps).
 */

/**
 * Create an encoder acceleration tracker
 * @param {Object} options Configuration options
 * @param {number} options.minStep - Minimum step size for slow turns (default: 1)
 * @param {number} options.maxStep - Maximum step size for fast turns (default: 10)
 * @param {number} options.slowThreshold - Time in ms for slowest speed (default: 200)
 * @param {number} options.fastThreshold - Time in ms for fastest speed (default: 30)
 * @returns {Object} Encoder acceleration tracker
 */
export function createEncoderAccel(options = {}) {
    const {
        minStep = 1,
        maxStep = 10,
        slowThreshold = 200,
        fastThreshold = 30
    } = options;

    /* Track last event time per encoder (by index or CC number) */
    const lastEventTime = new Map();

    /**
     * Get the accelerated step size for an encoder event
     * @param {number} encoderId - Encoder identifier (index or CC number)
     * @param {number} delta - Raw delta from encoder (-1 or +1)
     * @returns {number} Accelerated step value (signed, preserves direction)
     */
    function getAcceleratedDelta(encoderId, delta) {
        const now = Date.now();
        const lastTime = lastEventTime.get(encoderId) || 0;
        const elapsed = now - lastTime;
        lastEventTime.set(encoderId, now);

        /* Calculate step based on speed */
        let step;
        if (elapsed <= 0 || elapsed >= slowThreshold) {
            /* First event or very slow - use minimum step */
            step = minStep;
        } else if (elapsed <= fastThreshold) {
            /* Very fast - use maximum step */
            step = maxStep;
        } else {
            /* Interpolate between min and max based on speed */
            const speedRatio = (slowThreshold - elapsed) / (slowThreshold - fastThreshold);
            step = Math.round(minStep + speedRatio * (maxStep - minStep));
        }

        return delta * step;
    }

    /**
     * Reset tracking for an encoder (e.g., on touch release)
     * @param {number} encoderId - Encoder identifier
     */
    function reset(encoderId) {
        lastEventTime.delete(encoderId);
    }

    /**
     * Reset all encoder tracking
     */
    function resetAll() {
        lastEventTime.clear();
    }

    return {
        getAcceleratedDelta,
        reset,
        resetAll
    };
}

/* Default shared instance for convenience */
let defaultAccel = null;

/**
 * Get the default encoder acceleration instance
 * @returns {Object} Default encoder acceleration tracker
 */
export function getDefaultEncoderAccel() {
    if (!defaultAccel) {
        defaultAccel = createEncoderAccel();
    }
    return defaultAccel;
}
