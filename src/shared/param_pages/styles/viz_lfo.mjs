/**
 * styles/viz_lfo.mjs — SET 9: ten treatments of the LFO waveform.
 *
 * THE SHAPE IS NOT A VARIABLE. A sine is a sine, a saw ramps and stops, and a
 * square's edge is vertical — viz_draw.mjs has three long comments about how
 * hard it was to make those true on a 128x13 band, and none of that is this
 * issue's to relitigate. The ten options are the shared treatments from
 * viz_treatments.mjs, registered by reference, driven by heights this file
 * computes from `lfoShapeSample`, the same function `drawLfo` uses.
 *
 * THE ZERO LINE IS THE MIDDLE, and that is what makes this set the interesting
 * one. On the envelope and the filter the zero line is the floor, so "fill to
 * the baseline" and "fill to the bottom edge" are the same picture and
 * treatments 7 and 8 nearly collapse into each other. Here they are completely
 * different: a fill to the centre draws a SIGNED area that flips side every
 * half cycle, and a fill to the bottom edge draws a skyline. If the four sets
 * disagree about which treatment wins, this is the set where the disagreement
 * will come from.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { VIZ_ROWS, drawLfo, lfoShapeSample } from "../viz_draw.mjs";
import { treatmentOptions } from "./viz_treatments.mjs";

/** Bipolar: the wave swings either side of a centre line, and does not mirror
 * — a saw is not symmetric about its own axis and drawing it as if it were
 * would be a different waveform. */
const OPTS = { baseFrac: 0.5, mirror: false };

/**
 * One height per column: the shape sample mapped from -1..1 onto 0..1, so that
 * 0.5 is the zero line and `depth` scales the swing about it.
 *
 * `cycles` follows `drawLfo`'s own rate curve (1 + sqrt(rate) * 7) so the
 * density on screen is the density the shipping graph would show at the same
 * rate value, rather than a number picked to look nice here.
 */
export function lfoHeights(w, shape, rateFrac, depth, phase = 0) {
    const W = Math.max(2, w);
    const cycles = 1 + Math.sqrt(Math.max(0, Math.min(1, rateFrac))) * 7;
    const out = new Array(W);
    for (let i = 0; i < W; i++) {
        const t = (i / W) * cycles + phase;
        out[i] = 0.5 + 0.5 * lfoShapeSample(shape, t) * depth;
    }
    return out;
}

/* SHAPE 0 IS SINE (see lfoShapeSample). The swatch is a sine at a rate that
 * gives about three and a half cycles across the row — enough that it reads as
 * periodic and not so many that a 13-row band turns each half cycle into three
 * pixels, which is where a fill treatment would be judged on aliasing rather
 * than on itself. */
const SHAPE = 0, RATE = 0.128, DEPTH = 0.9;

/* The second context row is a SAW at a slower rate. It is the shape where the
 * signed fill is most visible — half of every cycle is above the axis and half
 * below, and the flyback is a vertical wall through the whole band — and it is
 * also the one that shows whether a treatment survives a discontinuity. */
const SHAPE2 = 2, RATE2 = 0.05, DEPTH2 = 0.9;

const PROBE_W = 128;
const PROBE_RECT = { x: 0, y: 0, w: PROBE_W, h: VIZ_ROWS };
const PROBE_HEIGHTS = lfoHeights(PROBE_W, SHAPE, RATE, DEPTH);

const ROLES = { shape: "shape", rate: "rate", depth: "depth" };
const SHAPES = ["Sine", "Triangle", "Saw", "Square", "S+H"];
const VALUES = { shape: "Sine", rate: RATE, depth: DEPTH };
const VALUES2 = { shape: "Saw", rate: RATE2, depth: DEPTH2 };
/* Depth is SIGNED in the shipping renderer — a -1..1 range, where the sign
 * inverts the wave rather than shrinking it. Declaring it any other way puts
 * zero depth at half amplitude, which viz_draw's own comment calls out. */
const META = {
    getOrGuess: (key) => {
        if (key === "shape") return { key, type: "enum", kind: "enum", options: SHAPES };
        if (key === "depth") return { key, type: "float", kind: "number", min: -1, max: 1 };
        return { key, type: "float", kind: "number", min: 0, max: 1 };
    },
};

/** The NOW row: the shipping graph at the same shape, rate and depth. */
function baseline(ctx, rect, _heights, _opts, values) {
    drawLfo(ctx, rect, ROLES, values || VALUES, META);
}

const NOTES = {
    "thin-stroke": "A 1px wave with a 2x2 dot at each end. Close to what ships, and on the LFO the endpoint dots are the most obviously wrong part of it: a waveform has no start and no end, it is a window onto something periodic, so marking the two edges asserts a boundary that does not exist. Sitting at position 1 it is the incumbent under examination rather than the reference — the wave itself is fine, the punctuation is not.",
    "no-endpoints": "The dots removed, which for this graph is not a weight change but a correctness one: the wave now runs off both edges the way a periodic signal should, and nothing suggests the first and last columns are special. Of the four sets this is the one where option 2 has the strongest case over option 1, and it costs literally nothing. Its limit is that it is still a hairline on a page of filled widgets.",
    "baseline": "A solid rule along the zero line — down the MIDDLE of the band here, not along the floor. That is a real difference in kind from the other three sets: the axis now sits inside the wave, so at a low depth the rule and the curve overlap for most of the row and the graph reads as one thick line rather than as a wave near an axis. It also makes the polarity unmistakable at a glance, which is what viz_draw draws its own centre rule for.",
    "dotted-baseline": "The centre axis at 50%, which is what ships. The dots interleave with a shallow wave instead of merging with it, so the low-depth case that breaks the solid rule survives here — the axis stays legible as a separate object at every depth. This is the conservative pick for the LFO, and the objection is the same as ever: it changes nothing, so adopting it settles the ink question and not the derivation one.",
    "ghost-fill": "CHECKER between the wave and the CENTRE line, so the fill flips side every half cycle — a signed area rather than a silhouette. It is the only fill family that shows an LFO the way a modulation actually behaves, adding above the axis and subtracting below, and at 50% the alternating blocks are unmistakable. At a high rate each half cycle is only a few columns wide and a checker in a 6-row-tall wedge is four scattered pixels, so the treatment that reads best slowly reads worst fast.",
    "light-fill": "DIAG_LIGHT, signed to the centre. Light enough that the crest stays the dominant thing in the cell, which on a waveform matters more than on a filter — the wave IS the information and the fill is only there to say which side of the axis it is on. On a fast wave a 25% fill in a narrow lobe disappears entirely, and the option degrades gracefully into a plain stroke rather than into noise, which is more than can be said for CHECKER.",
    "hatch-fill": "DIAG_THIRD, signed to the centre. The density that keeps a lobe visible at rates where CHECKER has broken up, and the one that makes the alternating sign easiest to see because the diagonal runs one way against a rising flank and the other way against a falling one. It is the best of the three signed fills on this set by a clear margin, and it is also the heaviest thing on the page after solid-mass.",
    "terrain": "Solid crest over DIAG_THIRD, filled to the BOTTOM EDGE — a skyline, not a signed area. On the LFO this is the largest departure in the set: the wave stops being something that swings about an axis and becomes the top of a landscape, so polarity is no longer readable at all and the trough of every cycle is buried in fill. Against that, it is the most legible waveform here from across a room, it matches the vimana idiom the filter set also proposes, and for a unipolar LFO — which viz_draw already draws sitting on the floor — it is arguably the correct picture rather than a stylisation.",
    "solid-mass": "SOLID between the wave and the centre line, no crest. Alternating filled lobes above and below the axis, which is the classic filled-waveform look and the most immediately readable thing in this set. The failure is specific and bad: the axis itself is never drawn, so at a low depth the lobes are one or two rows tall and the cell reads as a dashed line, and at zero depth it draws NOTHING AT ALL — an LFO turned off and an LFO that failed to load are the same picture, which is the one reading a graph must never allow.",
    "outline-only": "The outline of those same signed lobes. Ink does not grow with depth, so a page keeps its weight, and the hollow lobes above and below the axis are a genuinely distinctive shape — nothing else in the fleet looks like it. It has the same zero-depth failure as solid-mass in a milder form (a flat line rather than nothing), and at a high rate the outlines of adjacent lobes are two columns apart and merge into a solid band, which is precisely the opposite of what the treatment is for.",
};

export function register() {
    return registerSet({
        id: "viz_lfo",
        title: "LFO — the waveform graph",
        kind: KIND_DRAW,
        replaces: "drawLfo",
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
                else draw(ctx, rect, lfoHeights(RM.W, SHAPE2, RATE2, DEPTH2), OPTS, VALUES2);
            }
        },
        options: treatmentOptions(NOTES),
    });
}
