/**
 * styles/viz_treatments.mjs — the ten curve TREATMENTS, shared by sets 7-10.
 *
 * Filter curves, ADSR envelopes, LFO waveforms and sample waveforms are
 * pictures of the maths. An ADSR that does not look like an ADSR is simply
 * wrong, and nobody owns the shape of an exponential decay — so unlike every
 * other set in this catalog, these four vary TREATMENT ONLY: stroke weight,
 * fill, baseline, how the mass under the curve is rendered.
 *
 * That constraint is enforced MECHANICALLY rather than by discipline. Every
 * function here takes a precomputed `heights` array — one normalised 0..1 value
 * per column — and has no access to the parameters that produced it, no idea
 * which of the four graphs it is drawing, and no way to bend the shape. The
 * four set modules import these functions and register them UNMODIFIED, so an
 * option is literally the same function object in all four sets: given the same
 * height array they must produce the same curve, and the only thing that can
 * differ is fill.
 *
 * Do not wrap a treatment in a set module. A wrapper is the one hole in this,
 * and it would let a set silently disagree with its siblings about a shape that
 * is not this issue's to change.
 *
 * ---------------------------------------------------------------- geometry --
 *
 * `rect` is the viz cell, `{x, y, w, h}`. Like viz_draw's own `band()`,
 * everything is drawn in topY = rect.y + 1 .. topY + VIZ_ROWS - 1 (13 rows).
 * Nothing may leave that band: a row of overflow lands on the label strip,
 * which the grid does not repaint.
 *
 * Two shape-independent facts come in through `opts` rather than being guessed:
 *
 *   baseFrac   where the ZERO LINE of this graph sits, 0 = bottom row, 0.5 =
 *              centre. An envelope and a filter response are unipolar and rest
 *              on the floor; an LFO and a sample waveform swing either side of
 *              a centre line. A fill has to know which, or it fills the wrong
 *              half of the cell.
 *   mirror     the graph is symmetric about its zero line — a sample waveform
 *              is, an LFO is not. The reflection is computed from the SAME
 *              height array, so it cannot introduce a shape either.
 *
 * Neither is a treatment variable: all four sets pass the same pair to all ten
 * options, so no option can win or lose on it.
 *
 * ------------------------------------------------------------ on 13 rows --
 *
 * The band is 13 rows and the fills are the risk. At a high, flat value a
 * pattern in a band this short stops reading as texture and reads as a solid
 * block — CHECKER over 12 rows is grey, but SOLID over 12 rows is a bar with a
 * curve-shaped lid, and the curve is what the cell is for. That is why 8 and 9
 * are ordered where they are, and why 10 exists at all: an outline is the only
 * treatment whose ink does not grow with the value.
 *
 * The other end of the same problem: 6 and 7 are DIAG_LIGHT and DIAG_THIRD,
 * 25% and 33%, which rendered are two diagonal hatches differing only in a
 * stripe pitch of 4 against 3. They separate on a large area and barely
 * separate at all in a four-row wedge. The pair is kept because it is the
 * established density ladder — the knob and fader sets already offer both, and
 * dropping one here would make position 6 mean something different in this set
 * than in those — but a preference between them should be read as weak unless
 * the margin is large.
 *
 * ------------------------------------------------------------ on notches --
 *
 * Corner notches are a keeper elsewhere in this catalog and are deliberately
 * ABSENT here. They belong on a box whose corners are a design decision; the
 * corners of a filled curve are DATA — the left edge of a filter's passband,
 * the floor a release lands on — and rounding them off would be exactly the
 * misrepresentation this whole set is built to prevent.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { VIZ_ROWS } from "../viz_draw.mjs";
import { SOLID, CHECKER, DIAG_LIGHT, DIAG_THIRD, dottedRule } from "./dither.mjs";

const clamp01 = (v) => (typeof v === "number" && v === v ? (v > 1 ? 1 : v < 0 ? 0 : v) : 0);

/**
 * Resolve a height array against a rect: one y per pixel column, plus the base
 * row and (optionally) the reflected curve.
 *
 * Resampling is PROPORTIONAL, not hold-the-last-value: a set generates its
 * heights at whatever resolution its maths is natural at, and a 128-sample
 * array drawn into a 32px cell must show the whole graph squeezed, not the
 * first quarter of it. `fillTerrain` in dither.mjs holds instead, which is
 * right for its callers (a fader passes one value per column already) and
 * wrong for a curve.
 */
export function vizColumns(rect, heights, opts = {}) {
    const topY = rect.y + 1;
    const botY = topY + VIZ_ROWS - 1;
    const H = botY - topY;
    const w = Math.max(1, rect.w | 0);
    const src = (heights && heights.length) ? heights : [0];
    const n = src.length;
    const yBase = Math.round(botY - clamp01(opts.baseFrac || 0) * H);
    const fit = (y) => (y < topY ? topY : (y > botY ? botY : y));

    const yc = new Array(w);
    const ym = opts.mirror ? new Array(w) : null;
    for (let i = 0; i < w; i++) {
        const hv = clamp01(Number(src[Math.min(n - 1, Math.floor((i * n) / w))]));
        yc[i] = fit(Math.round(botY - hv * H));
        if (ym) ym[i] = fit(2 * yBase - yc[i]);
    }
    return { x0: rect.x, w, topY, botY, H, yBase, yc, ym };
}

/**
 * The rows this column's MASS occupies: between the curve and its reflection
 * when the graph is symmetric, otherwise between the curve and the zero line.
 *
 * Note that for a unipolar graph (baseFrac 0) this is curve..floor, which is
 * what "area under the curve" means; for a bipolar one it is a SIGNED area, so
 * a trough below the centre line fills downward. That is the honest reading and
 * it is also the only one that keeps the fill attached to the curve — filling
 * always-downward-to-the-floor would detach the shape from its ink on every
 * negative half cycle.
 */
function massSpan(g, i) {
    const a = g.yc[i];
    const b = g.ym ? g.ym[i] : g.yBase;
    return a < b ? [a, b] : [b, a];
}

/**
 * Stroke a per-column path as horizontal runs plus vertical risers — the same
 * construction as viz_draw's `drawStepCurve`, which is not exported.
 *
 * The riser carries only the rows BETWEEN two runs. Spanning them inclusive
 * re-draws the row the run already covered, and every row comes out one column
 * too long: the curve reads as a chunky zigzag rather than as a line. viz_draw
 * has that comment too, for the same reason.
 */
function strokePath(ctx, x0, ys, color = 1) {
    const n = ys.length;
    if (!n) return;
    let runStart = 0, runY = ys[0];
    for (let i = 1; i < n; i++) {
        if (ys[i] === runY) continue;
        ctx.fillRect(x0 + runStart, runY, i - runStart, 1, color);
        if (ys[i] < runY) ctx.fillRect(x0 + i, ys[i], 1, runY - ys[i], color);
        else ctx.fillRect(x0 + i, runY + 1, 1, ys[i] - runY, color);
        runStart = i; runY = ys[i];
    }
    ctx.fillRect(x0 + runStart, runY, n - runStart, 1, color);
}

/** Both halves of the graph, or the one there is. */
function strokeAll(ctx, g) {
    strokePath(ctx, g.x0, g.yc);
    if (g.ym) strokePath(ctx, g.x0, g.ym);
}

/**
 * A 2x2 endpoint dot, clamped into the band and off the right edge.
 *
 * viz_draw's envelope marks its vertices this way (`dot`, a 2x2 fillRect). Only
 * the two ENDS are marked here: a treatment cannot know where a curve's
 * vertices are without knowing what the curve is, which is the whole point.
 */
function endDot(ctx, g, x, y) {
    const px = Math.max(g.x0, Math.min(x, g.x0 + g.w - 2));
    const py = Math.max(g.topY, Math.min(y, g.botY - 1));
    ctx.fillRect(px, py, 2, 2, 1);
}

function endDots(ctx, g) {
    const last = g.w - 1;
    endDot(ctx, g, g.x0, g.yc[0]);
    endDot(ctx, g, g.x0 + last - 1, g.yc[last]);
    if (g.ym) {
        endDot(ctx, g, g.x0, g.ym[0]);
        endDot(ctx, g, g.x0 + last - 1, g.ym[last]);
    }
}

/**
 * Fill the mass through a dither predicate. `toBottom` extends every column to
 * the bottom row of the band regardless of where the zero line is — the vimana
 * terrain idiom, where the graph is a skyline over ground rather than a
 * quantity plotted against an axis.
 *
 * Predicates take ABSOLUTE coordinates (see dither.mjs), so two adjacent filled
 * cells share one lattice and a moving curve does not shimmer as its fill
 * re-phases underneath it.
 */
function fillMass(ctx, g, pattern, toBottom = false) {
    for (let i = 0; i < g.w; i++) {
        const [a, b0] = massSpan(g, i);
        const b = toBottom ? g.botY : b0;
        const x = g.x0 + i;
        for (let y = a; y <= b; y++) if (pattern(x, y)) ctx.fillRect(x, y, 1, 1, 1);
    }
}

/* ================================================================ 1..4 --
 * Line only. The mass under the curve is not drawn at all; what changes is how
 * much scaffolding the line is given. */

/**
 * 1. thin-stroke — what ships: a 1px polyline with a 2x2 dot at each end.
 */
export function drawThinStroke(ctx, rect, heights, opts) {
    const g = vizColumns(rect, heights, opts);
    strokeAll(ctx, g);
    endDots(ctx, g);
}

/**
 * 2. no-endpoints — the same line with the endpoint dots removed.
 */
export function drawNoEndpoints(ctx, rect, heights, opts) {
    const g = vizColumns(rect, heights, opts);
    strokeAll(ctx, g);
}

/**
 * 3. baseline — the line over a solid zero rule spanning the cell.
 */
export function drawBaselineRule(ctx, rect, heights, opts) {
    const g = vizColumns(rect, heights, opts);
    ctx.fillRect(g.x0, g.yBase, g.w, 1, 1);
    strokeAll(ctx, g);
}

/**
 * 4. dotted-baseline — the same rule at 50%.
 */
export function drawDottedBaseline(ctx, rect, heights, opts) {
    const g = vizColumns(rect, heights, opts);
    dottedRule(ctx, g.x0, g.yBase, g.w, g.x0 % 2);
    strokeAll(ctx, g);
}

/* ================================================================ 5..7 --
 * The mass appears, at three densities, under an unchanged line. */

/**
 * 5. ghost-fill — CHECKER under the line.
 */
export function drawGhostFill(ctx, rect, heights, opts) {
    const g = vizColumns(rect, heights, opts);
    fillMass(ctx, g, CHECKER);
    strokeAll(ctx, g);
}

/**
 * 6. light-fill — DIAG_LIGHT under the line.
 */
export function drawLightFill(ctx, rect, heights, opts) {
    const g = vizColumns(rect, heights, opts);
    fillMass(ctx, g, DIAG_LIGHT);
    strokeAll(ctx, g);
}

/**
 * 7. hatch-fill — DIAG_THIRD under the line.
 */
export function drawHatchFill(ctx, rect, heights, opts) {
    const g = vizColumns(rect, heights, opts);
    fillMass(ctx, g, DIAG_THIRD);
    strokeAll(ctx, g);
}

/* ================================================================ 8..10 --
 * The mass stops being a decoration of the line. */

/**
 * 8. terrain — a solid crest over DIAG_THIRD, filled to the BOTTOM EDGE.
 */
export function drawTerrainCurve(ctx, rect, heights, opts) {
    const g = vizColumns(rect, heights, opts);
    fillMass(ctx, g, DIAG_THIRD, true);
    strokePath(ctx, g.x0, g.yc);
}

/**
 * 9. solid-mass — SOLID between the curve and its zero line, no separate crest.
 */
export function drawSolidMass(ctx, rect, heights, opts) {
    const g = vizColumns(rect, heights, opts);
    fillMass(ctx, g, SOLID);
}

/**
 * 10. outline-only — the silhouette of the mass, drawn as its boundary.
 *
 * Emitting the boundary directly rather than filling and then clearing the
 * interior: the two produce the same pixels, and clearing would punch holes in
 * whatever the grid had already drawn behind the cell if any rounding put a
 * cleared pixel outside the mass.
 */
export function drawOutlineOnly(ctx, rect, heights, opts) {
    const g = vizColumns(rect, heights, opts);
    const spans = new Array(g.w);
    for (let i = 0; i < g.w; i++) spans[i] = massSpan(g, i);
    for (let i = 0; i < g.w; i++) {
        const [a, b] = spans[i];
        const prev = i > 0 ? spans[i - 1] : null;
        const next = i < g.w - 1 ? spans[i + 1] : null;
        const x = g.x0 + i;
        for (let y = a; y <= b; y++) {
            const interior = y > a && y < b
                && prev && y >= prev[0] && y <= prev[1]
                && next && y >= next[0] && y <= next[1];
            if (!interior) ctx.fillRect(x, y, 1, 1, 1);
        }
    }
}

/**
 * The ten, in catalog order, as `{ position, id, name, draw }` — the fields the
 * registry validates. A set module spreads these into its own `options` and
 * adds nothing but its own `note` per position.
 *
 * Shared so that "option 7" is the SAME FUNCTION in all four sets. If this ever
 * has to be copied, the property the whole task rests on is gone.
 */
export const VIZ_TREATMENTS = [
    { position: 1, id: "thin-stroke", name: "Thin stroke", draw: drawThinStroke },
    { position: 2, id: "no-endpoints", name: "No endpoints", draw: drawNoEndpoints },
    { position: 3, id: "baseline", name: "Baseline", draw: drawBaselineRule },
    { position: 4, id: "dotted-baseline", name: "Dotted baseline", draw: drawDottedBaseline },
    { position: 5, id: "ghost-fill", name: "Ghost fill", draw: drawGhostFill },
    { position: 6, id: "light-fill", name: "Light fill", draw: drawLightFill },
    { position: 7, id: "hatch-fill", name: "Hatch fill", draw: drawHatchFill },
    { position: 8, id: "terrain", name: "Terrain", draw: drawTerrainCurve },
    { position: 9, id: "solid-mass", name: "Solid mass", draw: drawSolidMass },
    { position: 10, id: "outline-only", name: "Outline only", draw: drawOutlineOnly },
];

/**
 * Build a set's `options` from the shared treatments plus a per-position note.
 * `notes` is keyed by treatment id; a missing one is a validation error at
 * registration time rather than a silent blank, because `validateSet` requires
 * a note and the catalog prints it.
 *
 * This does NOT wrap `draw` — the function object is passed through by
 * reference, which is what makes the cross-set identity assertion possible.
 */
export function treatmentOptions(notes) {
    return VIZ_TREATMENTS.map((t) => ({
        position: t.position, id: t.id, name: t.name, draw: t.draw,
        note: notes[t.id],
    }));
}
