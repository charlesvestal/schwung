/**
 * anim_state.mjs — the per-key frame store every widget animation needs.
 *
 * The page renderer is STATELESS: every frame is computed from the values it
 * is handed, so nothing in it can know what a value was a moment ago or how
 * long ago it changed. That is the whole reason the SCH-50 animation catalog
 * could only offer rendered frame strips. This is the one thing that has to
 * exist before any of it can be real, and it is built once, here, rather than
 * arriving as a side effect of whichever animation lands first.
 *
 * WHAT IT COSTS, because both costs are real and neither is obvious.
 *
 * 1. It is fed from values the page ALREADY reads. A whole page render is
 *    ~1.68ms and a single IPC parameter read is ~2.8ms — a read costs more
 *    than redrawing the entire screen — so an animation that needed its own
 *    reads would cost more than the thing it decorates. `observe` takes the
 *    value the renderer was handed anyway.
 *
 * 2. An idle page currently costs ZERO draws. Anything that animates makes it
 *    draw every tick for the duration of every change. `settled()` exists so a
 *    caller can ask whether anything is still moving and go back to sleep when
 *    nothing is; without it the store would quietly convert an idle screen into
 *    a permanently redrawing one.
 *
 * TIME IS PASSED IN, NEVER READ. No Date.now() anywhere: the renderer is pure
 * with respect to the device, which is what lets the harness draw what the
 * device draws and lets a movie be rendered deterministically. The caller
 * supplies `now` in milliseconds, exactly as it already supplies `nowMs` for
 * the trigger-button flash.
 */

/** A fresh store. One per page controller; not global. */
export function createAnimState() {
    return { prev: new Map(), from: new Map(), since: new Map() };
}

/**
 * Record `value` for `key` at time `now`, and report the transition.
 *
 * Returns `{ from, to, t, moving }`:
 *   from    the value held before the most recent change (null the first time)
 *   to      the value now
 *   t       0..1 progress through `durationMs`, 1 once settled
 *   moving  true while t < 1
 *
 * `from` is the value BEFORE the change, not the previous frame's value — a
 * knob turned three detents in three frames is one transition from where it
 * started, not three. Retargeting mid-flight keeps the ORIGINAL `from` only if
 * the animation has settled; otherwise it re-bases to where the value visually
 * is right now, so a fast scroll does not snap backwards. (vimana's AnimCurve
 * does the same thing for the same reason.)
 */
export function observe(state, key, value, now, durationMs = 120) {
    if (!state) return { from: null, to: value, t: 1, moving: false };

    const prev = state.prev.get(key);
    if (prev === undefined) {
        state.prev.set(key, value);
        state.from.set(key, value);
        /* ALREADY PAST, not `now`. A key seen for the first time has not
         * changed — it has arrived — and stamping it with the current time
         * makes the whole page animate on its first frame and read as moving
         * for a full duration afterwards, which also holds `settled()` false
         * and keeps an idle page redrawing. */
        state.since.set(key, now - durationMs);
        return { from: null, to: value, t: 1, moving: false };
    }

    if (!Object.is(prev, value)) {
        /* Re-base to where it visually IS, not to where it was when the last
         * change started. Mid-flight retargeting is the common case on a knob
         * and using the stale origin makes the widget jump backwards before
         * going forwards. */
        const inflight = progress(state, key, now, durationMs);
        state.from.set(key, inflight < 1 ? interpolatedOrPrev(state, key, inflight) : prev);
        state.prev.set(key, value);
        state.since.set(key, now);
    }

    const t = progress(state, key, now, durationMs);
    return { from: state.from.get(key), to: value, t, moving: t < 1 };
}

function progress(state, key, now, durationMs) {
    const since = state.since.get(key);
    if (since === undefined || !(durationMs > 0)) return 1;
    const dt = now - since;
    if (!(dt >= 0)) return 1;              /* clock went backwards: settle */
    return dt >= durationMs ? 1 : dt / durationMs;
}

/* For a numeric value, where it visually sits mid-flight; otherwise the value
 * it was heading to, since a non-numeric cannot be part-way between. */
function interpolatedOrPrev(state, key, t) {
    const a = Number(state.from.get(key)), b = Number(state.prev.get(key));
    if (isFinite(a) && isFinite(b)) return a + (b - a) * t;
    return state.prev.get(key);
}

/**
 * Is anything still moving?
 *
 * The gate on redrawing an idle page. A caller that does not consult this pays
 * a draw per tick forever, which is the single largest cost of animating
 * anything here.
 */
export function settled(state, now, durationMs = 120) {
    if (!state) return true;
    for (const since of state.since.values()) {
        const dt = now - since;
        if (dt >= 0 && dt < durationMs) return false;
    }
    return true;
}

/** Ease-out. Fast off the mark, settling rather than arriving. */
export const easeOut = (t) => 1 - (1 - t) * (1 - t);

/** Linear interpolation, clamped, so a caller cannot overshoot by accident. */
export function lerp(a, b, t) {
    const u = t < 0 ? 0 : (t > 1 ? 1 : t);
    return a + (b - a) * u;
}
