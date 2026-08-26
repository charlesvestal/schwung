/**
 * styles/anim.mjs — SET 13: ten ways a control could REACT to a value change.
 *
 * This set is shaped differently from every other set in the catalog, and the
 * reason is worth stating before the options: THERE IS ALMOST NOTHING TO
 * DIFFERENTIATE FROM. The render layer is stateless. `renderPageMovy` is
 * handed a `values` map and paints a snapshot; there is no per-widget store,
 * no previous value, no frame counter, so there is no place for a widget to
 * remember where it was and ease towards where it is. The only animation
 * anywhere in the shipping param pages is the trigger button press, which this
 * issue explicitly leaves alone.
 *
 * So this set is greenfield rather than a re-draw of an existing idiom, and
 * each option is a BEHAVIOUR SPEC plus a rendered frame strip, not live
 * motion. Wiring any of them up means giving the renderer frame state — a
 * per-key previous value, an age in ticks, and a redraw that keeps running
 * while a widget is still settling. That is an architectural change with its
 * own cost (a page currently only repaints when something changed; motion
 * makes every tick a repaint for as long as any control is moving), and it
 * belongs to the implementation follow-up, not to this catalog.
 *
 * KNOWN LIMIT, stated rather than worked around: the human judges these from a
 * strip of stills. A strip shows the TRAJECTORY — how far behind the target
 * the control sits at each step — and it genuinely does not show speed, so two
 * options that differ only in duration look like the same option drawn at
 * different spacings. Read the rendered-strip notes on each option below
 * before trusting a preference between them.
 *
 * ---------------------------------------------------------------- payload --
 *
 * KIND_MOTION requires `frames(from, to, n)` and `behaviour`. `frames` is the
 * PRIMARY trajectory: exactly `n` values, starting at `from`, settled at `to`.
 * Three options (7 flash, 8 trail, 9 dissolve) are not trajectories at all —
 * their value snaps and the motion is an additional mark drawn alongside — so
 * they carry an optional SECONDARY channel, and both channels are plain data
 * so a renderer can ignore either one:
 *
 *   ghost(from, to, n)  -> array of n entries, each null or
 *                          { value, fill } where `fill` names a density from
 *                          dither.mjs (SOLID / DIAG_HEAVY / CHECKER /
 *                          DIAG_LIGHT). A second, fainter widget drawn at
 *                          `value` on the same cell.
 *
 *   overlay(from, to, n) -> array of n entries, each null or a string naming a
 *                          whole-cell treatment. Only "invert" is used.
 *
 * A consumer that only knows about `frames` renders these three as `instant`,
 * which is exactly right: without the secondary channel, that is what they
 * are.
 *
 * -------------------------------------------------------------- the axis --
 *
 *   1      no motion. The control, and the current behaviour.
 *   2-4    the value LAGS, by increasing amounts. Nothing else changes.
 *   5-6    the value lags AND misses, then recovers. Physical rather than
 *          merely damped.
 *   7-9    the value does not lag at all — an extra mark carries the change.
 *          These are the cheapest to implement of the nine, because the value
 *          path stays exactly as it is today.
 *   10     the value lags in a way that has nothing to do with the input:
 *          quantised steps regardless of how fast the encoder turned.
 *
 * THE SPRING NEEDS A CLAMP. `1 - exp(-t*d)*cos(t*f)` only ever APPROACHES 1,
 * so the last frame of a spring is a fraction short of the target. That is
 * invisible in a rendered strip — a knob a percent off its value looks
 * identical — and obvious on hardware, where it means a parameter that never
 * quite lands on the number the encoder was turned to. Every option here
 * therefore ends with an explicit `out[n - 1] = to`, and that is asserted
 * rather than trusted.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_MOTION } from "./index.mjs";

/* -------------------------------------------------------------- shapes --
 *
 * One ramp builder for every trajectory option, so the only thing that varies
 * between 2, 3, 4, 5, 6 and 10 is DURATION and EASING — which is what the
 * catalog is asking the human to compare. A per-option loop would let a
 * spelling difference masquerade as a design difference.
 *
 * `dur` is in frames and is a property of the OPTION, not of `n`. `n` is how
 * many frames the caller wants to look at; an option that settles in 4 frames
 * shows 4 of motion and the rest parked, which is the honest picture. If `n`
 * is shorter than `dur` the trajectory is truncated and the final frame still
 * lands on `to` — a hard cut, but a settled one.
 */
function ramp(from, to, n, dur, ease) {
    if (!(n > 0)) return [];
    const out = new Array(n);
    for (let i = 0; i < n; i++) {
        if (i === 0) out[i] = from;
        else if (i >= dur) out[i] = to;
        else out[i] = from + (to - from) * ease(i / dur);
    }
    /* THE SETTLE CLAMP. See the header: an easing that only approaches its
     * target is invisible here and wrong on hardware. n === 1 cannot satisfy
     * both ends at once, and frame 0 wins — a one-frame strip is a "before"
     * picture, not a transition. */
    if (n > 1) out[n - 1] = to;
    return out;
}

const LINEAR = (t) => t;
const EASE_OUT = (t) => 1 - (1 - t) * (1 - t);

/*
 * easeOutBack, with c1 pushed from the conventional 1.70158 to 2.6.
 *
 * At the textbook constant the peak is ~1.10 of the travel, which over a 0.15
 * -> 0.85 change puts the overshoot frame at 0.92 against a target of 0.85 —
 * seven hundredths, or about one pixel of pointer at r=8. It is a real
 * overshoot that is invisible at the size this widget is drawn. At 2.6 the
 * peak is ~1.19, which is ~2px of pointer travel past the target, and it still
 * stays inside 0..1 for a full-range change (peak 1.19 of a 0 -> 1 travel
 * would clip, and a clipped overshoot reads as no overshoot; the option is
 * honest about this in its note).
 */
const C1 = 2.6, C3 = C1 + 1;
const EASE_BACK = (t) => { const u = t - 1; return 1 + C3 * u * u * u + C1 * u * u; };

/*
 * A damped oscillator. d is the decay, f the angular frequency over the unit
 * duration: f = 2*pi*1.25 is one and a quarter cycles, so the strip shows an
 * overshoot, an undershoot and the settle rather than one lonely bounce that
 * would be indistinguishable from option 5.
 *
 * d = 4.2 keeps the first peak at ~1.19 — deliberately the same height as
 * option 5, so the two options differ in WHAT HAPPENS AFTER the peak and not
 * in how hard they hit it. That is the comparison worth putting to a human.
 */
const SPRING_D = 4.2, SPRING_F = 2 * Math.PI * 1.25;
const EASE_SPRING = (t) => 1 - Math.exp(-t * SPRING_D) * Math.cos(t * SPRING_F);

/* --------------------------------------------------------- trajectories --
 *
 * THE SET IS FOUR, NOT TEN, AND THE SIX THAT WERE CUT ARE STILL HERE.
 *
 * Ten were authored and six were withdrawn from the catalog before any human
 * judged them, because a still frame strip cannot carry what separates them:
 *
 *   ease-2 / ease-4 / ease-8   differ ONLY in duration, and a strip renders
 *                              duration as a frame COUNT. A person comparing
 *                              them is reading a number off a chart, not
 *                              feeling a motion.
 *   spring                     shares its first peak with overshoot by
 *                              construction; what distinguishes it is an
 *                              undershoot of roughly one pixel of pointer
 *                              angle, invisible below about 9x.
 *   tick                       is only visible on a change SMALLER than the
 *                              frame count. Over the catalog sweep each frame
 *                              advances about two quanta and it reads as a
 *                              plain linear slide, so the strip UNDERSELLS it.
 *   instant                    is legible, but only as "nothing happens".
 *
 * Shipping those into a pairwise comparator would have produced preference
 * data that looked real and was not: the human would rank the picture, and the
 * picture is not the thing. They are exported rather than deleted because the
 * follow-up issue has to give the renderer per-key frame state anyway, and at
 * that point duration can be settled on hardware where it can be felt. The
 * spring in particular carries a settle clamp that was expensive to get right
 * and is invisible in any still.
 */

const framesInstant = (from, to, n) => ramp(from, to, n, 1, LINEAR);
const framesOvershoot = (from, to, n) => ramp(from, to, n, 6, EASE_BACK);

/* Withdrawn from the catalog; kept for the on-hardware follow-up. */
export const framesEase2 = (from, to, n) => ramp(from, to, n, 2, LINEAR);
export const framesEase4 = (from, to, n) => ramp(from, to, n, 4, EASE_OUT);
export const framesEase8 = (from, to, n) => ramp(from, to, n, 8, EASE_OUT);
export const framesSpring = (from, to, n) => ramp(from, to, n, 10, EASE_SPRING);

/**
 * 10. tick — the value crosses in fixed quantised steps.
 *
 * Quantised in the VALUE domain, not the time domain: the trajectory is
 * linear, then each frame is snapped to the nearest 1/TICK_Q of full scale, so
 * a small change lands in one step and a large one visibly clicks through
 * several. That is the property the option is for — the motion is a function
 * of the distance travelled rather than of how fast the encoder was turned, so
 * a slow careful turn and a fast flick produce the same march.
 *
 * The final frame is the exact target, unquantised. A control that could only
 * ever rest on a sixteenth of its range would be a different feature (and a
 * destructive one).
 *
 * The quantised frames are CLAMPED to the span being travelled. Rounding to
 * the nearest sixteenth rounds AWAY from the target as often as towards it, so
 * a 0.15 -> 0.85 change marched up to 0.875, sat there for four frames and
 * then stepped back down to land — an overshoot nobody asked for, in the one
 * option whose whole character is that it does not overshoot. Clamping is the
 * fix rather than flooring, because flooring is direction-dependent and would
 * need its own branch for a value going down.
 */
const TICK_Q = 16;
/* Withdrawn from the catalog; kept for the on-hardware follow-up. */
export function framesTick(from, to, n) {
    const raw = ramp(from, to, n, 6, LINEAR);
    const lo = Math.min(from, to), hi = Math.max(from, to);
    for (let i = 1; i < raw.length - 1; i++) {
        const q = Math.round(raw[i] * TICK_Q) / TICK_Q;
        raw[i] = q < lo ? lo : q > hi ? hi : q;
    }
    return raw;
}

/* ---------------------------------------------------- secondary channels --
 *
 * 7, 8 and 9 all snap the value, so all three share `framesInstant` and differ
 * only here. Keeping the value path identical is the point: these are the
 * three options that could be implemented without the renderer ever having to
 * interpolate anything, only to remember one previous value and an age.
 */

/** A ghost of the OLD position, fading through the density ladder. */
function ghostDecay(fills) {
    return (from, to, n) => {
        const out = new Array(Math.max(0, n)).fill(null);
        if (!(n > 0)) return [];
        /* No ghost when nothing moved: a mark that appears on a change that
         * did not happen is worse than no mark. */
        if (from === to) return out;
        for (let i = 0; i < fills.length && i + 1 < n; i++)
            out[i + 1] = { value: from, fill: fills[i] };
        return out;
    };
}

const ghostTrail = ghostDecay(["SOLID", "DIAG_HEAVY", "CHECKER", "DIAG_LIGHT"]);
const ghostDissolve = ghostDecay(["SOLID", "CHECKER", "DIAG_LIGHT"]);

/** The whole cell inverts for the two frames after the change. */
function overlayFlash(from, to, n) {
    const out = new Array(Math.max(0, n)).fill(null);
    if (!(n > 0)) return [];
    if (from === to) return out;
    for (let i = 1; i <= 2 && i < n; i++) out[i] = "invert";
    return out;
}

/**
 * Called from the bottom of index.mjs rather than running on import — see the
 * cycle note there.
 */
export function register() {
    return registerSet({
    id: "anim",
    title: "Value-change motion — how a control reacts when its value moves",
    kind: KIND_MOTION,
    replaces: "nothing — the render layer is stateless and has no motion to replace",
    optionCount: 4,
    options: [
        {
            position: 1, id: "overshoot", name: "Overshoot", frames: framesOvershoot,
            behaviour: "Eases PAST the target by about a fifth of the travel, then settles back over six frames. The control reads as having weight — it was thrown rather than moved — and the overshoot itself is what makes the change noticeable without any extra mark being drawn.",
            note: "Overshoot has to be clipped at the ends of travel, and a clipped overshoot is no overshoot at all: at the top of a range the pointer simply parks on the stop. So the effect is present in the middle of a range and silently absent at both extremes, which is where a value is most often being nudged. That inconsistency is the argument against it.",
        },
        {
            position: 2, id: "flash", name: "Flash", frames: framesInstant, overlay: overlayFlash,
            behaviour: "The value snaps — no lag at all — and the whole cell inverts for two frames. The change is announced rather than animated, so the number under the pointer is never wrong, which is the objection to every option from 2 to 6.",
            note: "Cheapest of the nine to implement: the value path is exactly what ships today and the renderer only needs to know that a key changed and how many ticks ago. Its risk is the opposite of subtlety — eight knobs turned in sequence is eight cells strobing, and inversion is the loudest thing available on a 1-bit display.",
        },
        {
            position: 3, id: "trail", name: "Trail", frames: framesInstant, ghost: ghostTrail,
            behaviour: "The value snaps, and a ghost of the PREVIOUS position is left behind, fading SOLID -> DIAG_HEAVY -> CHECKER -> DIAG_LIGHT over four frames. The motion is carried by a second mark rather than by the value, so the control shows both where it is and where it came from.",
            note: "The only option that conveys DIRECTION as well as change — a ghost behind the pointer says the value went up. Its problem is geometric and the rendered strip shows it plainly: on the arc knob, a 0.15 -> 0.85 change puts ghost and pointer at opposite ends of a near-horizontal diameter, and the two strokes read as ONE bar drawn straight through the dial rather than as a pointer with a trail behind it. A small turn fails the other way, with the ghost merging into the pointer as a fat stroke. Either way it needs a widget with somewhere to put a second mark, which is a constraint on the knob set as much as on this one.",
        },
        {
            position: 4, id: "dissolve", name: "Dissolve", frames: framesInstant, ghost: ghostDissolve,
            behaviour: "The same construction as Trail, one step quieter and one frame shorter: the previous position fades SOLID -> CHECKER -> DIAG_LIGHT and is gone in three frames. Uses the established density ladder as a time axis, which is an idiom this UI already has and does not currently use for anything temporal.",
            note: "Deliberately paired with Trail so the human is asked one question rather than two — is a decaying ghost worth having at all, and if so how loud. Densities on a 1-bit display are the only volume control available, so if either of these wins the follow-up question is only where on the ladder to sit. In the rendered strip the difference between the two is a single frame and one rung of the ladder, and it is very nearly invisible; that is a warning about the pair, not about the idea.",
        },
    ],
    });
}
