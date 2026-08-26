/**
 * styles/viz_sample.mjs — SET 10: ten treatments of the sample waveform.
 *
 * THE SHAPE IS NOT A VARIABLE. The ten options are the shared treatments from
 * viz_treatments.mjs, registered by reference, driven by an amplitude envelope
 * this file computes with the same formula `drawSample` uses in viz_draw.mjs.
 *
 * A caveat that belongs in the record rather than in a footnote: there is no
 * decoded audio here. `drawSample` says so itself — WAV parsing is its own
 * larger task — so the envelope below is a representative shape, not a file.
 * That does not weaken the comparison, because all four sets and all ten
 * options see the same array, and the question being asked is what a waveform
 * TREATMENT should look like, not what any particular sample looks like.
 *
 * THIS IS THE ONLY MIRRORED SET. A sample waveform is symmetric about its zero
 * line — that is what a waveform IS, a positive and a negative excursion of the
 * same signal — so `mirror` is on and every treatment draws the reflection from
 * the same height array. The consequence is that the set is the densest of the
 * four by construction: the mass here is the whole body between the two
 * envelopes rather than the area under one curve, so a treatment that is light
 * on the filter can be heavy here. That is real information about the
 * treatments, not a flaw in the set.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { VIZ_ROWS, drawSample } from "../viz_draw.mjs";
import { treatmentOptions } from "./viz_treatments.mjs";

/** Centred and symmetric — the two facts that make a waveform a waveform. */
const OPTS = { baseFrac: 0.5, mirror: true };

/**
 * The upper envelope as heights: 0.5 at silence, 1.0 at full scale, mapped so
 * that the mirror lands at the matching negative excursion.
 *
 * The inner formula is `drawSample`'s own `halfAt` — a hump across the whole
 * span times a fast ripple, which gives both a macro shape and a per-column
 * jitter. The jitter is the part that makes this look like audio rather than
 * like an envelope, and it is also the part that stresses a fill: adjacent
 * columns differ by a row or two, so a dither pattern is being asked to fill a
 * ragged region rather than a smooth one.
 */
export function sampleHeights(w, gain = 1) {
    const W = Math.max(2, w);
    const out = new Array(W);
    for (let i = 0; i < W; i++) {
        const t = i / W;
        const v = Math.abs(Math.sin(t * Math.PI)) * (0.55 + 0.35 * Math.sin(t * 23)) * gain;
        out[i] = 0.5 + 0.5 * Math.max(0, Math.min(1, v));
    }
    return out;
}

const PROBE_W = 128;
const PROBE_RECT = { x: 0, y: 0, w: PROBE_W, h: VIZ_ROWS };
const PROBE_HEIGHTS = sampleHeights(PROBE_W, 1);

/* The second context row is the same waveform at a THIRD of the gain — a quiet
 * sample, where the whole body is four rows tall and every treatment has to
 * work in a third of the band. It is the case that separates the fills that
 * degrade from the fills that vanish. */
const QUIET = 0.34;

const ROLES = { position: "position" };
const VALUES = { position: 0.42 };
const META = { getOrGuess: (key) => ({ key, type: "float", kind: "number", min: 0, max: 1 }) };

/** The NOW row: the shipping waveform, including its inverted position marker
 * — which none of the ten options draw, because a marker is not a treatment of
 * the curve and adding one to all ten would only add ink to every row. */
function baseline(ctx, rect) {
    drawSample(ctx, rect, ROLES, VALUES, META);
}

const NOTES = {
    "thin-stroke": "The upper and lower envelopes as two 1px lines with a 2x2 dot at each end. This is the outline of a waveform rather than a waveform — and it is worth saying plainly that it is NOT what ships: `drawSample` draws solid columns, so on this set alone position 1 is the radical option and position 9 is the incumbent. The axis still runs minimal-to-radical in TREATMENT terms, which is what makes option 7 comparable across the four sets, but the ranking here should not be read as distance from the current screen.",
    "no-endpoints": "The two envelopes with the dots removed. Same argument as the LFO and stronger: a sample window has no distinguished first and last column, and a 2x2 dot at the left edge sits exactly where the fade-in is, which is the one part of the shape it can obscure. Cheap, correct, and still a hairline pair on a page of filled widgets.",
    "baseline": "A solid rule down the centre between the two envelopes. It reinstates the zero line the outline treatments throw away, and on a quiet sample — where the two envelopes are two rows apart — it is the only thing keeping the pair from reading as one thick smear. It also fills the middle of the body at every amplitude, so the graph is never hollow at the centre, which is either honest or misleading depending on whether you read the centre as zero or as signal.",
    "dotted-baseline": "The same centre line at 50%. Half the ink and the same rescue on a quiet sample, and it matches the dotted axis the LFO and filter graphs already draw — picking this across all four sets gives the fleet one axis idiom. On a loud sample it is invisible under the body, so it costs nothing where it is not needed, which is the best behaviour a piece of chrome can have.",
    "ghost-fill": "CHECKER through the whole body between the two envelopes. A waveform at 50% density is a grey band with a ragged edge, and the raggedness — the per-column jitter that makes it look like audio — survives better than it does under a solid, because the boundary reads against a texture rather than against a block. On a quiet sample the body is four rows and a checker in four rows is a dotted smudge.",
    "light-fill": "DIAG_LIGHT through the body. The lightest treatment that still says there is something between the envelopes, and the one that keeps the two 1px crests clearly separate from their own fill at every amplitude — which on a mirrored graph is harder than it sounds, since there are two boundaries competing with one fill. At a third gain the fill is two or three pixels per column and effectively disappears, leaving the outline pair from option 2.",
    "hatch-fill": "DIAG_THIRD through the body. On a mirrored graph this is noticeably heavier than the same treatment on the filter — the mass is the whole body, not the area under one curve — so the density that was the safe middle over there is closer to the top of the range here. That is the honest cost of sharing an axis across four graphs, and it is also the finding: a treatment cannot be judged once and applied everywhere.",
    "terrain": "Solid crest over DIAG_THIRD, filled to the BOTTOM EDGE. The lower envelope is inside the fill and stops existing as a boundary, so the waveform becomes a skyline — its bottom half is not drawn as a shape at all, only as ground. For a sample that is a genuine loss of information (a waveform is symmetric and showing one half asserts it is not) and a genuine gain in legibility. It is also the heaviest thing in the whole catalog on this set: a QUIET sample has its crest near the centre line, so seven of the thirteen rows are hatch and the cell renders as a textured slab with a faint ripple on top. Of the ten, this is the one whose merit depends most on whether the cell is being read or merely recognised.",
    "solid-mass": "SOLID through the whole body — which is what ships. `drawSample` draws exactly this, one filled column per pixel between the two envelopes, so on this set option 9 is the incumbent rather than the extreme. It is the most legible waveform in the set, it is the reading every DAW uses, and its weakness is the one the file header names: at full scale a 13-row band goes solid and the shape survives only as a notch in the lid.",
    "outline-only": "The boundary of the body: the two envelopes joined by an end wall at each edge, hollow inside. Ink is independent of amplitude, and on a ragged waveform the outline is a genuinely striking shape — the jitter that is invisible inside a solid becomes the whole picture. Two failures worth weighing, and the first is worse than it sounds: where the body narrows to one or two rows the outline has no interior, so it collapses back to a solid — and on a mirrored graph the collapsed result is a pair of envelope lines, which is option 2. Rendered side by side at a quiet gain the two are nearly the same picture, and this option only separates from it where the sample is loud. The second is that the end walls are an artefact of enclosure rather than part of the signal.",
};

export function register() {
    return registerSet({
        id: "viz_sample",
        title: "Sample — the waveform body",
        kind: KIND_DRAW,
        replaces: "drawSample",
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
                draw(ctx, rect, sampleHeights(RM.W, row === 0 ? 1 : QUIET), OPTS);
            }
        },
        options: treatmentOptions(NOTES),
    });
}
