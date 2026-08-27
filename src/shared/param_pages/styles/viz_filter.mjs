/**
 * styles/viz_filter.mjs — SET 8: ten treatments of the filter response graph.
 *
 * THE SHAPE IS NOT A VARIABLE, and here that is not a preference — it is a
 * correctness rule. The graph is a picture of what the filter does; an option
 * that bent the roll-off, softened the resonant peak or flattened the shoulder
 * would make the screen misreport the audio. The issue that spawned this
 * catalog names filter curves as inherent to the product experience for exactly
 * this reason.
 *
 * So the ten options are the shared treatments from viz_treatments.mjs,
 * registered by reference, and every one of them receives a height array this
 * file computes from `filterGainAt` — the same function `drawFilter` calls in
 * viz_draw.mjs, ported verbatim from schwung-movy. No option can reach the
 * maths.
 *
 * ONE HONEST DIFFERENCE FROM THE SHIPPING RENDERER. `drawFilter` terminates the
 * polyline at the point where the curve crosses into the floor, so a lowpass
 * stops where it dies instead of running along the bottom row (see the long
 * comment on `drawColumnCurve` — that termination is a real bug fix, not
 * decoration). A treatment renders every column of the array it is given, so
 * the tail here lies on the floor as a flat run. That is a property of the
 * shared-array contract, it is identical in all ten options, and it therefore
 * cannot bias the comparison; on the fill treatments it is invisible, since a
 * zero-height column has no mass either way.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { VIZ_ROWS, drawFilter, filterGainAt } from "../viz_draw.mjs";
import { treatmentOptions } from "./viz_treatments.mjs";

/** A magnitude response rests on the floor; there is no negative gain. */
const OPTS = { baseFrac: 0, mirror: false };

/** One gain per column, straight out of the shipping `filterGainAt`. */
export function filterHeights(w, mode, cutoff, resonance, steep) {
    const W = Math.max(2, w);
    const out = new Array(W);
    for (let i = 0; i < W; i++) out[i] = filterGainAt(i / W, mode, cutoff, resonance, steep);
    return out;
}

/* The swatch: a lowpass a little past centre with real resonance, which is the
 * shape that exercises everything the graph can show — a flat passband, a
 * shoulder, a peak, and a roll-off that reaches the floor inside the cell. */
const CUT = 0.55, RES = 0.55;

/* The second context row is a BANDPASS at high resonance: a narrow spike over
 * an otherwise empty band. It is the case where a fill has almost no area to
 * work with and where an outline has to enclose something four columns wide. */
const CUT2 = 0.42, RES2 = 0.85;

const PROBE_W = 128;
const PROBE_RECT = { x: 0, y: 0, w: PROBE_W, h: VIZ_ROWS };
const PROBE_HEIGHTS = filterHeights(PROBE_W, "lp", CUT, RES, false);

/*
 * The shipping renderer takes a mode as an option STRING and resolves it
 * through `filterModeOf`, so the meta here declares the option list a real
 * module would and the value is the spelling a real module reports.
 */
const ROLES = { mode: "mode", cutoff: "cutoff", resonance: "resonance" };
const MODES = ["Lowpass", "Highpass", "Bandpass", "Notch"];
const VALUES = { mode: "Lowpass", cutoff: CUT, resonance: RES };
const VALUES2 = { mode: "Bandpass", cutoff: CUT2, resonance: RES2 };
const META = {
    getOrGuess: (key) => (key === "mode"
        ? { key, type: "enum", kind: "enum", options: MODES }
        : { key, type: "float", kind: "number", min: 0, max: 1 }),
};

/** The NOW row: the shipping graph at the same cutoff and resonance. */
function baseline(ctx, rect, _heights, _opts, values) {
    drawFilter(ctx, rect, ROLES, values || VALUES, META);
}

const NOTES = {
    "thin-stroke": "A 1px response curve with a 2x2 dot at each end. This is what ships, minus the shipping renderer's floor termination — a hairline that has to carry a flat passband, a resonant shoulder and a roll-off all at once. It works, and the reason to question it is that on this graph the LINE is the least of what matters: a filter is read as how much of the spectrum survives, and a boundary with nothing under it makes you infer that instead of showing it.",
    "no-endpoints": "The dots removed. On the filter they are worse than on the envelope: a lowpass at a low cutoff has its whole curve in the left third, so the right-hand dot sits alone on the floor at the far edge with no line near it and reads as a stray mark rather than as the end of anything. Dropping them costs nothing here — the passband already starts at the left edge, so there is no ambiguity about where the graph begins.",
    "baseline": "A solid rule along the floor. It gives the roll-off a surface to arrive at, which is the one thing this graph most needs and the shipping renderer already agrees with — `drawFilter` draws a dotted floor before anything else. Solid is heavier than what ships, and heavy is a real cost on a graph whose curve can occupy as little as a fifth of the cell: at a low cutoff the axis becomes the most prominent thing in the picture.",
    "dotted-baseline": "The floor at 50%, which is what ships. Same reading as the solid rule at half the ink, and it is quiet enough to sit under a curve that runs almost along it without the two merging. This is the conservative pick for the filter specifically, and its weakness is that it changes nothing at all — adopting it answers the weight question and not the derivation one.",
    "ghost-fill": "CHECKER under the response. The graph becomes the spectrum that survives, which is the correct mental model and a genuinely different reading from a boundary line. At 50% over a 12-row passband it is grey rather than textured, so a wide-open lowpass fills most of the cell; against that, the resonant peak stops being a wiggle in a line and becomes a visible bulge of extra area, which is what resonance actually does.",
    "light-fill": "DIAG_LIGHT. The passband stays clearly a mass while remaining light enough that the 1px crest on top of it is unambiguous, which matters more on this graph than on any of the other three because the crest is the frequency response and the fill is only an aid to reading it. On a narrow bandpass a 25% fill four columns wide is one or two lit pixels, so the option that helps most on a lowpass helps least on the mode that needs it.",
    "hatch-fill": "DIAG_THIRD. Dense enough to survive a narrow bandpass and light enough not to swallow the crest on a wide-open lowpass — on this set it is the fill that fails in neither direction, which none of the other two manage. It also inherits the vimana filter page's own idiom directly: that reference draws its filter response as exactly this, a solid crest over a 1-in-3 hatch, and it is the closest thing in the catalog to a precedent rather than a hypothesis.",
    "terrain": "The vimana construction proper: solid crest, DIAG_THIRD, filled to the bottom edge. On the filter the bottom edge IS the zero line, so this differs from hatch-fill only in drawing the crest over its own fill rather than beside it — a one-pixel distinction here, and a large one on the LFO and sample sets where the zero line is the centre of the band. Voting for it on this set alone is close to voting for 7 twice, which is a real weakness of sharing one axis across four graphs.",
    "solid-mass": "SOLID under the curve. The strongest reading of the four modes at a glance — a lowpass is a block on the left, a highpass a block on the right, a bandpass a spike, a notch a bite out of a bar — and the strongest failure at the top of the range: a filter wide open is a 12-row solid bar across the whole cell, and the resonant shoulder that distinguishes it from a filter that is simply bypassed is a single pixel of lid. Best silhouette, worst headroom.",
    "outline-only": "The silhouette of the passband, hollow. Ink is independent of cutoff, so a wide-open filter costs no more than a closed one and the page weight never changes with the sound. The shape it draws is also unusually apt on this graph: enclosing the passband draws the band edges as real vertical walls, and a bandpass becomes a closed shape you can see the width of. The artefact is the same as ever — the walls at the extreme left and right are not part of the response, and on a lowpass the left wall is the y axis by accident rather than by intent.",
};

export function register() {
    return registerSet({
        id: "viz_filter",
        title: "Filter — the response curve",
        kind: KIND_DRAW,
        replaces: "drawFilter",
        probeSize: { w: PROBE_W, h: 15 },
        probe: (ctx, draw) => draw(ctx, PROBE_RECT, PROBE_HEIGHTS, OPTS),
        baseline,
        context: (ctx, draw, info) => {
            const RM = info.RM;
            const rows = new Set(info.slots.map((s) => (s < 4 ? 0 : 1)));
            for (const row of rows) {
                const rowY = row === 0 ? RM.ROW0_Y : RM.ROW1_Y;
                ctx.fillRect(0, rowY, RM.W, RM.BOX_H, 0);
                const rect = { x: 0, y: rowY, w: RM.W, h: VIZ_ROWS };
                if (row === 0) draw(ctx, rect, PROBE_HEIGHTS, OPTS, VALUES);
                else draw(ctx, rect, filterHeights(RM.W, "bp", CUT2, RES2, false), OPTS, VALUES2);
            }
        },
        options: treatmentOptions(NOTES),
    });
}
