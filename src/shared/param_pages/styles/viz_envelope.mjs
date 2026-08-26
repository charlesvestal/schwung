/**
 * styles/viz_envelope.mjs — SET 7: ten treatments of the ADSR graph.
 *
 * THE SHAPE IS NOT A VARIABLE. An ADSR that does not look like an ADSR is
 * wrong, and nobody owns the shape of a linear attack into a decay into a
 * plateau. So this set — and its three siblings, filter, LFO and sample — vary
 * TREATMENT only, and the ten treatments live in viz_treatments.mjs and are
 * registered here by reference. Given the same height array, option 7 in this
 * set and option 7 in the filter set are the same function producing the same
 * pixels.
 *
 * What this file owns is the HEIGHTS: one 0..1 value per column, computed from
 * the same geometry `drawFullAdsr` uses in viz_draw.mjs (a 26/128 attack span,
 * a 4+24/128 decay, a note-off at 88/128, a 33/128 release), so the curve the
 * options render is the curve that ships.
 *
 * Nothing here ships. Production draw code imports nothing from this file.
 */

import { registerSet, KIND_DRAW } from "./index.mjs";
import { VIZ_ROWS, drawEnvelope } from "../viz_draw.mjs";
import { treatmentOptions } from "./viz_treatments.mjs";

/** An envelope rests on the floor and has no negative half. */
const OPTS = { baseFrac: 0, mirror: false };

/**
 * The ADSR silhouette as heights, from `drawFullAdsr`'s reference geometry.
 *
 * Every breakpoint is expressed against W exactly as the shipping renderer
 * does, including the two clamps — the decay cannot cross the note-off, and the
 * release cannot leave the cell. Reproducing those matters more than it looks:
 * without the decay clamp a long decay pushes the plateau past the gate and the
 * graph shows a sustain that never happens.
 */
export function adsrHeights(w, a, d, s, r) {
    const W = Math.max(2, w);
    const gateX = W * (88 / 128);
    const peakX = a * W * (26 / 128);
    let sustX = peakX + W * (4 / 128) + d * W * (24 / 128);
    if (sustX > gateX - W * (2 / 128)) sustX = gateX - W * (2 / 128);
    let relEndX = gateX + W * (4 / 128) + r * W * (33 / 128);
    if (relEndX > W - 1) relEndX = W - 1;

    const out = new Array(W);
    for (let i = 0; i < W; i++) {
        let v;
        if (i <= peakX) v = peakX <= 0 ? 1 : i / peakX;
        else if (i <= sustX) v = sustX <= peakX ? s : 1 + (s - 1) * ((i - peakX) / (sustX - peakX));
        else if (i <= gateX) v = s;
        else if (i <= relEndX) v = relEndX <= gateX ? 0 : s * (1 - (i - gateX) / (relEndX - gateX));
        else v = 0;
        out[i] = v;
    }
    return out;
}

/* The swatch envelope: a slow-ish attack, a real decay, a mid plateau and a
 * medium release — the shape that exercises all four segments at once. */
const A = 0.55, D = 0.60, S = 0.55, R = 0.50;

/* The second context row is a LONG RELEASE at a low sustain, which is the case
 * that collapses: the plateau is two rows off the floor and the release is a
 * shallow ramp into it, so a heavy fill has almost nothing to fill and a light
 * one has almost no ink. If a treatment fails anywhere it fails here. */
const A2 = 0.10, D2 = 0.30, S2 = 0.18, R2 = 1.0;

const PROBE_W = 128;
const PROBE_RECT = { x: 0, y: 0, w: PROBE_W, h: VIZ_ROWS };
const PROBE_HEIGHTS = adsrHeights(PROBE_W, A, D, S, R);

/* Drives the shipping renderer for the NOW row, at the same four values the
 * heights above were built from — so the baseline and the options are the same
 * envelope and only the treatment differs. */
const ROLES = { attack: "attack", decay: "decay", sustain: "sustain", release: "release" };
const VALUES = { attack: A, decay: D, sustain: S, release: R };
const VALUES2 = { attack: A2, decay: D2, sustain: S2, release: R2 };
const META = { getOrGuess: (key) => ({ key, type: "float", kind: "number", min: 0, max: 1 }) };

/** The NOW row. Ignores `heights` because the shipping renderer computes its
 * own — that is exactly the difference between it and the ten options. */
function baseline(ctx, rect, _heights, _opts, values) {
    drawEnvelope(ctx, rect, ROLES, values || VALUES, META);
}

const NOTES = {
    "thin-stroke": "What ships, near enough: a 1px polyline with a 2x2 dot at each end. The envelope is the one graph in the fleet whose shape is fully legible as a line — four straight segments, no curvature to lose — so the honest position is that this treatment already works and the set exists to find out whether anything reads BETTER, not to fix something broken. Its weakness is uniformity: on a page where every other cell is a filled widget, a hairline graph reads as unpopulated at a glance.",
    "no-endpoints": "The same line with the endpoint dots removed. The dots are viz_draw's own vertex marks reduced to the two ends, and at 13 rows a 2x2 dot is a sixth of the band tall — on a fast attack it merges with the rising segment and thickens the corner into a blob. Removing them is the smallest possible change and it makes the attack corner sharper. It also removes the only thing that says where the graph STARTS when the attack is instant and the first column is already at full height.",
    "baseline": "A solid rule along the zero line under the stroke. For an envelope the zero line is the floor, so this draws the note-off level explicitly and gives the release somewhere to land instead of fading into an empty row. It is the cheapest way to make a hairline graph look deliberate. The cost is that a solid full-width rule is 128 lit pixels of pure chrome, which on a two-row page is more ink than most of the actual curves.",
    "dotted-baseline": "The same rule at 50%. Half the chrome for the same reading, and it matches the dotted axis viz_draw already draws under the filter and LFO graphs — so this is the treatment that makes the four graphs agree with each other rather than each inventing its own floor. At an ADSR with a long release the shallow tail runs almost parallel to the rule for a third of the cell, and at that separation the dots and the curve interleave into a texture rather than reading as two lines.",
    "ghost-fill": "CHECKER under the curve. The envelope becomes an area, which is the reading a musician actually wants — how much of the note is loud — rather than a boundary they have to integrate by eye. 50% is the highest density that still lets the 1px stroke on top stay visibly separate from its own fill. At a high sustain it covers most of a 13-row band, and a 12-row checker at true size is grey, not texture: the page gets noticeably heavier.",
    "light-fill": "DIAG_LIGHT, a quarter density. The lightest fill that still says solid-under-the-line, and the one that survives the long-release case best because a 2-row-tall mass at 25% is still visibly a mass while the same band at CHECKER is three stray pixels. The objection is the opposite of the one above: at a low sustain over a narrow release, 1-in-4 is sparse enough to read as dirt on the screen rather than as fill.",
    "hatch-fill": "DIAG_THIRD, between the two. It is also the density the vimana reference uses under a crest, so picking it here and terrain at 8 gives the same ladder across the whole catalog. On an envelope specifically it has one real advantage: the diagonal runs against the attack ramp and with the release ramp, so the two flanks are distinguishable by their moire even when they are the same height, which neither CHECKER nor a solid can do.",
    "terrain": "A solid crest over DIAG_THIRD, filled to the BOTTOM EDGE of the band rather than to the zero line. For an envelope those are the same row, so this and hatch-fill differ only in the crest being drawn solid over its own fill — the difference is small here and large on the LFO and sample sets, where the zero line is the centre. Choosing it is really a vote about those; on this set alone it is nearly a duplicate of 7, which is an honest weakness of a shared axis.",
    "solid-mass": "SOLID between the curve and the floor, with no separate crest. The most legible option in the set at a distance and the most reductive: the graph becomes a silhouette, so a low sustain is a thin wedge and a high one is a filled block with a notch cut out of the top left. At a sustain near full the 13-row band is a solid bar and the decay segment — a two-row step in the lid — is the only thing distinguishing it from a filled rectangle. That is the failure the file header warns about and this option is where it lands.",
    "outline-only": "The boundary of the filled silhouette: the curve, its two vertical ends, and the floor under it, with nothing inside. It is the only treatment whose ink does not grow with the value, so a full-sustain envelope costs the same as an empty one and a page of eight never gets heavier. The price is a closed shape — the vertical end walls are not part of the envelope, they are an artefact of enclosing it, and at a fast attack the left wall and the attack ramp are the same two pixels.",
};

export function register() {
    return registerSet({
        id: "viz_envelope",
        title: "Envelope — the ADSR graph",
        kind: KIND_DRAW,
        replaces: "drawEnvelope",
        /* A full row, not a cell: the ADSR group spans all four slots of a row
         * whenever it has four roles, which is the layout this graph is
         * designed for and the only one where the release has room. h is BOX_H
         * (15) so the band's last row is inside the surface and the shipping
         * widget does not report itself as clipping. */
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
                /* Row 0 the characteristic envelope, row 1 the long-release
                 * collapse case — one page, both readings. */
                if (row === 0) draw(ctx, rect, PROBE_HEIGHTS, OPTS, VALUES);
                else draw(ctx, rect, adsrHeights(RM.W, A2, D2, S2, R2), OPTS, VALUES2);
            }
        },
        options: treatmentOptions(NOTES),
    });
}
