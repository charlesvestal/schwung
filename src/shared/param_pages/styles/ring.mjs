/**
 * styles/ring.mjs — the hand-tuned circle the arc knobs are drawn on.
 *
 * WHY THIS EXISTS.
 *
 * `ctx.drawArc(cx, cy, r, start, sweep)` — and `js_display_draw_arc` in the
 * device C that it mirrors — is a distance-rounded UNION of two scans: one
 * pixel per row (the x whose distance rounds to r) and one pixel per column.
 * The column pass is what keeps the ring 8-connected, and it is also what
 * ruins it: near the top and bottom the circle is nearly horizontal, so five
 * consecutive columns all round to the same y and the cap comes out five
 * pixels wide and dead flat. At r=6:
 *
 *     row  1: 5 px   <- flat cap
 *     row  2: 2 px
 *      ...           (2px sides all the way down)
 *     row 13: 5 px   <- flat cap
 *
 * The shape is symmetric — there is no left/right bug — but a radius-6 circle
 * with a 5px flat top and bottom reads as a ROUNDED RECTANGLE. Every one of
 * the arc-family knob options inherited that, which is why they were all
 * reported as "sloppy": the defect was in the primitive, not in the designs.
 *
 * WHY TABULATED.
 *
 * `drawSwitch` in viz_draw.mjs already answers this question for this codebase:
 * it is "a tabulated sprite ported pixel-for-pixel from Movy ... because it is
 * a circle and a circle rasterised at one size cannot be stretched to another
 * and stay round." At these radii an algorithm cannot produce a clean circle,
 * which is exactly why bitmap fonts and icon sets are drawn rather than
 * generated. So the outlines below are authored by hand, one run per row.
 *
 * Two side effects worth having:
 *   - Nothing here calls `ctx.drawArc`, so there is no native-vs-fallback
 *     divergence left to preview wrong. Everything is `fillRect`.
 *   - The cap is 3px and the shoulder rows carry an explicit connectivity
 *     filler, so the outline is 8-connected without a column pass.
 *
 * ANGLE ADDRESSING.
 *
 * The options need partial rings — option 1's track is open at the bottom,
 * option 3 draws only the travelled span, option 10 punches a notch at the
 * value. Rather than invent a second, angle-aware rasteriser, each shape is
 * expanded ONCE into a list of pixels each carrying its own angle, cached by
 * key, and a partial draw is a filter over that list. Consequences:
 *
 *   - a partial draw is a strict SUBSET of the whole shape, pixel for pixel,
 *     so a fill can never disagree with the track it grows along;
 *   - the ends of a span are a clean radial cut rather than whatever the
 *     rasteriser happened to keep at that radius;
 *   - the same list serves a 1px ring and an n-px band, so the tuned outer
 *     silhouette is shared by both.
 *
 * Angles use the display convention: 0 at twelve o'clock, increasing
 * clockwise, i.e. `atan2(dx, -dy)`. That is what `drawArc` uses, so the
 * shipping 230-degree start / 260-degree sweep geometry carries over
 * unchanged.
 *
 * BANDS. A band's OUTER edge is the hand-authored silhouette of `rOut` (so a
 * thick track has the same profile as a thin one). Its INNER edge is computed
 * at the half-integer boundary `(rIn - 0.5)`, i.e. every pixel whose true
 * radius rounds to `rIn` or more. Taking the inner edge from the hand table
 * instead would drag in the shoulder FILLERS of the inner ring — pixels that
 * exist for connectivity, not because they are at that radius — and flare the
 * band by a pixel either side of the caps.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

/**
 * Hand-authored outlines. `RING_RUNS[r][|dy|] = [lo, hi]` is the inclusive x
 * run on the RIGHT half of row `dy`; the left half is its mirror and rows
 * above centre mirror rows below, so every ring is symmetric in both axes by
 * construction rather than by inspection.
 *
 * Read a column downward and you can see the two rules the tables follow:
 *
 *   1. the cap row is `[0, 1]` — three pixels wide after mirroring, never the
 *      five drawArc gives or the one true geometry gives;
 *   2. the row above the cap is a RUN rather than a single pixel (r=8 row 7 is
 *      `[2, 4]`, not `[4, 4]`), because the cap sits one pixel proud of the
 *      circle and without the filler the outline breaks 8-connectivity there.
 *
 * Everywhere else the value is `round(sqrt(r*r - dy*dy))` — the honest circle.
 * Only the last two rows are drawn by hand, and they are the only two rows
 * where an algorithm gets it wrong.
 */
export const RING_RUNS = {
    8: [[8, 8], [8, 8], [8, 8], [7, 7], [7, 7], [6, 6], [5, 5], [2, 4], [0, 1]],
    7: [[7, 7], [7, 7], [7, 7], [6, 6], [6, 6], [5, 5], [2, 4], [0, 1]],
    6: [[6, 6], [6, 6], [6, 6], [5, 5], [4, 4], [2, 3], [0, 1]],
    5: [[5, 5], [5, 5], [5, 5], [4, 4], [2, 3], [0, 1]],
    4: [[4, 4], [4, 4], [3, 3], [1, 2], [0, 1]],
};

/** Smallest x >= 0 whose true radius at row `dy` rounds to `r` or more. */
function innerLo(r, dy) {
    const rr = (r - 0.5) * (r - 0.5) - dy * dy;
    return rr <= 0 ? 0 : Math.ceil(Math.sqrt(rr));
}

/** Display-convention angle of an offset: 0 at twelve o'clock, clockwise. */
function angleOf(dx, dy) {
    let a = Math.atan2(dx, -dy) * 180 / Math.PI;
    return a < 0 ? a + 360 : a;
}

const CACHE = new Map();

/**
 * The pixel list for the annulus `rIn..rOut`, as flat triples (dx, dy, angle).
 *
 * Flat rather than an array of objects because it is walked per draw per knob
 * per frame in the shipping geometry this catalog is proposing for, and a
 * three-field object per pixel is ~100 allocations a widget for nothing.
 */
function pixels(rIn, rOut) {
    const key = rIn * 16 + rOut;
    const hit = CACHE.get(key);
    if (hit) return hit;

    const outer = RING_RUNS[rOut];
    const out = [];
    if (outer) {
        for (let dy = -outer.length + 1; dy <= outer.length - 1; dy++) {
            const run = outer[Math.abs(dy)];
            const hi = run[1];
            /* A 1px ring keeps its authored lo (fillers and all); a band takes
             * the geometric inner edge. See the header. */
            const lo = rIn >= rOut ? run[0] : Math.min(innerLo(rIn, dy), hi);
            for (let x = lo; x <= hi; x++) {
                out.push(x, dy, angleOf(x, dy));
                if (x !== 0) out.push(-x, dy, angleOf(-x, dy));
            }
        }
    }
    CACHE.set(key, out);
    return out;
}

function inSweep(a, start, sweep) {
    if (sweep >= 360) return true;
    let d = a - ((start % 360) + 360) % 360;
    if (d < 0) d += 360;
    return d <= sweep;
}

function plot(ctx, cx, cy, rIn, rOut, start, sweep, color) {
    if (sweep <= 0) return;
    const p = pixels(rIn, rOut);
    const whole = sweep >= 360;
    for (let i = 0; i < p.length; i += 3) {
        if (whole || inSweep(p[i + 2], start, sweep))
            ctx.fillRect(cx + p[i], cy + p[i + 1], 1, 1, color);
    }
}

/** A 1px hand-tuned ring, whole or between two angles. */
export function ring(ctx, cx, cy, r, start = 0, sweep = 360, color = 1) {
    plot(ctx, cx, cy, r, r, start, sweep, color);
}

/** A solid annulus from `rIn` to `rOut` inclusive, whole or between two angles. */
export function ringBand(ctx, cx, cy, rIn, rOut, start = 0, sweep = 360, color = 1) {
    if (rOut < rIn) return;
    plot(ctx, cx, cy, Math.max(1, rIn), rOut, start, sweep, color);
}

/** The radii this module has tables for, smallest first. Handy for tests. */
export const RING_RADII = Object.keys(RING_RUNS).map(Number).sort((a, b) => a - b);
