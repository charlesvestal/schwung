/**
 * dither.mjs — the density ladder shared by every option that fills an area.
 *
 * On a 1-bit display, "colour" is pixel density. One ladder with fixed
 * meanings is what makes a set of options read as one design language rather
 * than as ten unrelated fills:
 *
 *   100%  SOLID       active, selected, primary
 *    75%  DIAG_HEAVY  emphasised secondary
 *    50%  CHECKER     muted, ghosted
 *    25%  DIAG_LIGHT  background, hint, range
 *    11%  DOTS(3)     reference grid, barely there
 *
 * Predicates take ABSOLUTE screen coordinates, not rect-relative ones. That is
 * deliberate: phase-continuity across adjacent shapes means two neighbouring
 * filled areas share one lattice instead of showing a seam, and a shape that
 * moves does not shimmer as its fill re-phases under it.
 *
 * Adapted from the density ladder in vimana2-rust
 * (crates/vimana-app/src/anim.rs, docs/plans/aesthetic-reference.md).
 */

export const SOLID = (_x, _y) => true;
export const CHECKER = (x, y) => ((x + y) % 2) === 0;
export const DIAG_LIGHT = (x, y) => ((x + y) % 4) === 0;
export const DIAG_HEAVY = (x, y) => ((x + y) % 4) !== 0;
export const DIAG_THIRD = (x, y) => ((x + y) % 3) === 0;
export const DOTS = (n = 3) => (x, y) => (x % n) === 0 && (y % n) === 0;

/** Nominal densities, for tests and for documenting an option. */
export const NOMINAL = { SOLID: 1, DIAG_HEAVY: 0.75, CHECKER: 0.5, DIAG_THIRD: 1 / 3, DIAG_LIGHT: 0.25, DOTS3: 1 / 9 };

/**
 * Fill a rect through a pattern. Only ever SETS pixels — a dithered fill
 * composites over whatever is beneath it rather than punching a hole in it.
 */
export function fillDithered(ctx, x, y, w, h, pattern) {
    for (let dy = 0; dy < h; dy++) {
        for (let dx = 0; dx < w; dx++) {
            const px = x + dx, py = y + dy;
            if (pattern(px, py)) ctx.fillRect(px, py, 1, 1, 1);
        }
    }
}

/**
 * Fill from a curve down to the bottom edge of the rect, so the shape reads as
 * mass rather than as a line. `heights` is one normalised 0..1 value per
 * column; short arrays hold their last value.
 *
 * `crest` draws a solid 1px line along the curve itself. Without it a light
 * pattern makes the boundary ambiguous, which is the whole reason the vimana
 * filter page strokes the crest solid over a 1-in-3 hatch.
 */
export function fillTerrain(ctx, x, y, w, h, heights, pattern, crest = true) {
    if (!heights || !heights.length) return;
    for (let i = 0; i < w; i++) {
        const hv = Math.max(0, Math.min(1, heights[Math.min(i, heights.length - 1)]));
        const curveY = y + h - Math.round(hv * h);
        const bottom = y + h - 1;
        if (crest && curveY <= bottom) ctx.fillRect(x + i, curveY, 1, 1, 1);
        for (let py = curveY + (crest ? 1 : 0); py <= bottom; py++) {
            if (pattern(x + i, py)) ctx.fillRect(x + i, py, 1, 1, 1);
        }
    }
}

/** A 50%-dotted rule — the separator idiom. Horizontal. */
export function dottedRule(ctx, x, y, w, phase = 0) {
    for (let i = 0; i < w; i++) if (((i + phase) % 2) === 0) ctx.fillRect(x + i, y, 1, 1, 1);
}

/** A dashed vertical rule. `dash` on, `gap` off. */
export function dashedVRule(ctx, x, y, h, dash = 1, gap = 1) {
    const cycle = dash + gap;
    for (let i = 0; i < h; i++) if ((i % cycle) < dash) ctx.fillRect(x, y + i, 1, 1, 1);
}

/** Knock the four corner pixels out of a box. The "rounded" idiom. */
export function notchCorners(ctx, x, y, w, h) {
    ctx.fillRect(x, y, 1, 1, 0);
    ctx.fillRect(x + w - 1, y, 1, 1, 0);
    ctx.fillRect(x, y + h - 1, 1, 1, 0);
    ctx.fillRect(x + w - 1, y + h - 1, 1, 1, 0);
}
