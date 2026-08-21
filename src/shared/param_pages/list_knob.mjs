/**
 * list_knob.mjs — turning a KNOB to scroll a list.
 *
 * The jog and a knob are not the same input, and treating them the same is
 * what this fixes. A jog detent is a deliberate click you feel; a knob detent
 * is a fraction of a casual twist, and there are dozens of them in one flick
 * of the wrist. So 1:1 — correct for the jog — reads as "way too fast" on a
 * knob, reported from the device.
 *
 * Two rules, and the second is the one that makes long lists usable:
 *
 *   1. A slow, deliberate turn costs DETENTS_PER_ENTRY detents per entry, so
 *      you can land on the one you want.
 *   2. A fast turn accelerates — but only as far as the list is long. A
 *      6-option enum never accelerates at all (there is nowhere to go); a
 *      519-entry airwindows list can move 16 entries a step, which crosses it
 *      in about a hundred detents instead of fifteen hundred.
 *
 * Length-aware acceleration is the whole trick. A fixed multiplier is either
 * useless on the long lists or uncontrollable on the short ones, and the fleet
 * has both: Braids has 47 models, `clap` has 519 effects, and a Recv Ch has
 * 17.
 *
 * PURE and injected-clock, so the feel is testable without a device — see
 * tests/host/test_list_knob.sh.
 */

/** Detents of knob travel per list entry at the slowest, most deliberate turn. */
export const DETENTS_PER_ENTRY = 3;

/**
 * Entries per unit of acceleration ceiling: a list of N can move up to N/8
 * entries per step when spun fast.
 *
 * One constant does both jobs. It sets how hard a long list accelerates AND,
 * because the ceiling floors to 1 below 8 entries, it is why a short list does
 * not accelerate at all — there is nowhere to go, and a 6-option enum that
 * jumped 3 at a time would be unusable.
 *
 * Calibrated against the real fleet rather than a round number: Braids has 47
 * models (ceiling 5, so a fast spin crosses it in ~28 detents), `clap` has 519
 * effects (ceiling capped at 16, ~98 detents), a Recv Ch has 17 (ceiling 2 —
 * present but gentle, since you are usually going one or two places).
 */
export const ACCEL_DIVISOR = 8;

/** Hard ceiling on entries per step, however long the list or fast the turn. */
export const ACCEL_MAX_MULTIPLIER = 16;

/* Same-direction gaps under these many ms count as a fast turn. Two bands
 * rather than a curve: the ear for this is coarse, and two are tunable by
 * someone reading the numbers. */
const FAST_MS = 60;
const BRISK_MS = 140;

export function listKnobInit() {
    return { accum: 0, lastStepMs: 0, lastDir: 0 };
}

/**
 * How far a knob turn should move the highlight.
 *
 * @param {object} state   from listKnobInit(), mutated
 * @param {number} delta   raw detents this event, signed
 * @param {number} nowMs   monotonic-ish clock
 * @param {number} length  number of entries in the list
 * @returns {number} entries to move, signed; 0 while the turn is still banking
 */
export function listKnobStep(state, delta, nowMs, length) {
    if (!state || !delta) return 0;

    /* Reversing drops the banked partial turn. Without it the residue is spent
     * cancelling the new direction first, so a reversal costs more than a
     * fresh turn and costs a DIFFERENT amount depending on where the last one
     * stopped — the same inconsistency #228 fixed on the grid. */
    if (state.accum !== 0 && Math.sign(state.accum) !== Math.sign(delta)) state.accum = 0;

    state.accum += delta;
    let steps = Math.trunc(state.accum / DETENTS_PER_ENTRY);
    if (steps === 0) return 0;
    state.accum -= steps * DETENTS_PER_ENTRY;

    const dir = Math.sign(steps);
    const elapsed = state.lastStepMs > 0 ? nowMs - state.lastStepMs : Infinity;

    /* How much acceleration this list can even use. Floors to 1 — i.e. none —
     * for anything shorter than ACCEL_DIVISOR entries. */
    const n = Math.max(0, length | 0);
    const ceiling = Math.min(ACCEL_MAX_MULTIPLIER,
                             Math.max(1, Math.floor(n / ACCEL_DIVISOR)));

    let mult = 1;
    if (dir === state.lastDir && ceiling > 1) {
        if (elapsed <= FAST_MS) mult = ceiling;
        else if (elapsed <= BRISK_MS) mult = Math.max(1, Math.floor(ceiling / 2));
    }

    state.lastStepMs = nowMs;
    state.lastDir = dir;
    return steps * mult;
}
