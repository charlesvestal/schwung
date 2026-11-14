/*
 * knob_direction_filter.mjs -- OPT-IN smoothing for an encoder that
 * misreports its direction.
 *
 * A worn or dirty encoder does not skip counts, it reports some of them
 * BACKWARDS, in bursts. Measured on a real Move (2026-10-22, knob 1, raw in
 * the shim): turned one way, it gave 78-83% correct counts at slow speed with
 * wrong-way runs of up to ~14 -- enough to walk a 4-detent enum backwards by
 * three options. Fast turns were nearly clean. A one- or two-count debounce
 * does nothing against runs that long.
 *
 * The filter keeps the last WINDOW counts per knob and an accepted direction.
 * A count in the accepted direction passes; a count against it is held back.
 * The accepted direction flips only once the window leans the other way by
 * MARGIN, and a new gesture (reset() on touch-down) commits only once START
 * counts agree. The price is paid by DELIBERATE motion: a reversal shorter
 * than MARGIN is swallowed and a longer one arrives late -- which is why this
 * is off unless a device asks for it.
 *
 * Off by default: a healthy knob gains nothing and pays the reversal lag.
 * Enabled per device by the file /data/UserData/schwung/knob_direction_filter
 * (see parseKnobFilterConfig). tests/host/test_knob_direction_filter.sh runs
 * the captured stream through the real knob engine.
 */

export const KNOB_FILTER_WINDOW = 12;
export const KNOB_FILTER_MARGIN = 6;
/* A new touch commits to a direction only once this many counts agree: both
 * captured clockwise turns OPENED with a wrong-way burst (-5, -6), and letting
 * the first count decide walked the enum backwards before the turn began. */
export const KNOB_FILTER_START = 8;
export const KNOB_FILTER_FLAG_PATH = "/data/UserData/schwung/knob_direction_filter";

export function createKnobDirectionFilter({ window = KNOB_FILTER_WINDOW, margin = KNOB_FILTER_MARGIN,
                                            start = KNOB_FILTER_START } = {}) {
    const hist = new Map();      /* knob -> array of +1/-1, newest last */
    const accepted = new Map();  /* knob -> +1 | -1 | 0 */

    function reset(knob) { hist.delete(knob); accepted.delete(knob); }

    /* `delta` is the signed relative value (decodeDelta); a +-2 counts twice. */
    function accept(knob, delta) {
        const s = delta > 0 ? 1 : delta < 0 ? -1 : 0;
        if (!s) return false;
        let h = hist.get(knob);
        if (!h) { h = []; hist.set(knob, h); }
        for (let i = 0; i < Math.abs(delta); i++) h.push(s);
        if (h.length > window) h.splice(0, h.length - window);
        let acc = accepted.get(knob) || 0;
        let sum = 0;
        for (const x of h) sum += x;
        if (acc === 0) { if (Math.abs(sum) >= start) acc = sum > 0 ? 1 : -1; }
        else if (sum * acc < 0 && Math.abs(sum) >= margin) acc = sum > 0 ? 1 : -1;
        accepted.set(knob, acc);
        return acc !== 0 && s === acc;
    }

    return { accept, reset };
}

/*
 * The flag file's contents -> which knobs are filtered.
 *   null/undefined (no file)  -> off
 *   "" (empty file)           -> all eight knobs
 *   "1" / "1,4"               -> those knobs (1-based, as printed on nothing,
 *                                counted left to right); anything else ignored
 * Slots are returned 0-based, the grid's numbering.
 */
export function parseKnobFilterConfig(text) {
    if (text === null || text === undefined) return { enabled: false, knobs: new Set() };
    const knobs = new Set();
    for (const tok of String(text).split(/[^0-9]+/)) {
        const n = parseInt(tok, 10);
        if (n >= 1 && n <= 8) knobs.add(n - 1);
    }
    if (!knobs.size) for (let i = 0; i < 8; i++) knobs.add(i);
    return { enabled: true, knobs };
}
