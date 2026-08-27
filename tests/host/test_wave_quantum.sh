#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE TRIANGLE IS DRAWN AT A WIDTH ITS SLOPE DIVIDES.
#
# What reads as jagged is not the step SIZE, it is the step size CHANGING. A
# triangle traverses 4*amp = 24 rows per cycle, so its staircase is uniform only
# when the drawn width is a multiple of 24.
#
# SO THE ASSERTION IS UNIFORMITY, NOT PIXELS. A baseline would pass just as
# happily with the quantum wrong by a factor of two, because the picture would
# still be a triangle. This measures the property the change exists for, and
# checks the counterfactual -- that the unquantized width is NOT uniform --
# so the test cannot pass vacuously.
#
# MEASURED FROM THE SAMPLER, NOT FROM THE FRAMEBUFFER. The stroke shares yAt
# with the CHECKER mass fill, and CHECKER lights (x+y)%2===0 -- so a
# topmost-lit-pixel probe reads the dither alternating column by column and
# reports every run as length 1, for every shape, whatever the geometry. That
# probe was written first and it looked like it worked.
#
# THE WIDTHS ARE THE ONES THAT EXIST: the grid cell (CELL_W 32) and the knob
# card cell (29), each less the 2px pad drawWaveform insets on both sides. The
# saw was in the quantum table until it was measured against these instead of
# against a round 30 -- it gains slightly at one and LOSES at the other. That is
# pinned below as an absence, because a test that only checked "the triangle is
# uniform" would pass with the saw wrongly included.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the wave quantum test" >&2
  exit 1
fi

node --input-type=module -e '
import { lfoShapeSample, drawWaveform, VIZ_ROWS } from "./src/shared/param_pages/viz_draw.mjs";
import { CELL_W } from "./src/shared/param_pages/render_page_movy.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const AMP = (VIZ_ROWS - 1) / 2;
const PAD = 2;                 /* drawWaveform insets this on each side */
const CELLS = [CELL_W, 29];    /* knob grid, knob card */
const TRI = 1, SINE = 0, SAW = 2, SQUARE = 3;

/* Column runs of the stroke, from the sampler the stroke traces. A level run
 * (the top of a square) is not a step, so it is dropped. */
function runs(shape, w) {
    const ys = [];
    for (let px = 0; px < w; px++)
        ys.push(Math.round(AMP - lfoShapeSample(shape, px / w) * AMP));
    const out = [];
    for (const y of ys) {
        if (out.length && out[out.length - 1][0] === y) out[out.length - 1][1]++;
        else out.push([y, 1]);
    }
    return out.map((r) => r[1]).filter((n) => n < w / 2);
}
const uniform = (a) => a.length > 0 && Math.max(...a) === Math.min(...a);
const show = (a) => { const h = {}; for (const n of a) h[n] = (h[n] || 0) + 1; return JSON.stringify(h); };

/* The span the renderer actually draws into, off a real framebuffer, because
 * the point is the CALL SITE and not the arithmetic. */
function drawnSpan(shapeIndex, cellW) {
    const W = cellW, H = 15, px = new Uint8Array(W * H);
    const ctx = { fillRect(x, y, w, h, c) {
        for (let j = y; j < y + h; j++)
            for (let i = x; i < x + w; i++)
                if (i >= 0 && i < W && j >= 0 && j < H) px[j * W + i] = c ? 1 : 0;
    } };
    const metaIndex = { getOrGuess: () => ({ type: "enum", kind: "enum",
        options: ["sine", "tri", "saw", "square"] }) };
    drawWaveform(ctx, { x: 0, y: 0, w: W, h: H }, "s",
        { s: String(shapeIndex) }, metaIndex, null, undefined);
    let lo = -1, hi = -1;
    for (let i = 0; i < W; i++) {
        let lit = false;
        for (let j = 0; j < H; j++) if (px[j * W + i]) { lit = true; break; }
        if (lit) { if (lo < 0) lo = i; hi = i; }
    }
    return { lo, hi, span: hi - lo + 1 };
}

const TRI_Q = 4 * AMP;

for (const cell of CELLS) {
    const full = cell - PAD * 2;
    const want = Math.floor(full / TRI_Q) * TRI_Q;

    /* 1. Quantized is uniform. */
    if (!uniform(runs(TRI, want)))
        bad("cell " + cell + ": triangle at " + want + "px should have ONE step size, saw " +
            show(runs(TRI, want)));

    /* 2. The counterfactual -- the cell width is NOT uniform. Without this the
     *    first assertion could hold for reasons unrelated to the quantum. */
    if (want !== full && uniform(runs(TRI, full)))
        bad("cell " + cell + ": triangle at the full " + full + "px came out uniform too, " +
            "so the quantum is not what produces uniformity and this test proves nothing");

    /* 3. The renderer applies it, and centres what it gives up. */
    const t = drawnSpan(TRI, cell);
    if (t.span !== want)
        bad("cell " + cell + ": triangle drawn span " + t.span + ", expected " + want);
    const left = t.lo - PAD, right = (cell - PAD - 1) - t.hi;
    if (Math.abs(left - right) > 1)
        bad("cell " + cell + ": triangle not centred, " + left + "px left vs " + right + "px right");

    /* 4. THE EXCLUSION. Everything else keeps the whole cell. The saw is the
     *    one that looks like it belongs in the table and does not. */
    for (const [shape, name] of [[SINE, "sine"], [SAW, "saw"], [SQUARE, "square"]]) {
        const s = drawnSpan(shape, cell);
        if (s.span !== full)
            bad("cell " + cell + ": " + name + " should keep the full " + full +
                "px, saw " + s.span + " -- only the triangle is uniform at every " +
                "width tested; quantizing the saw loses width at " + cell +
                " for " + show(runs(SAW, Math.floor(full / (2 * AMP)) * (2 * AMP))) +
                " against " + show(runs(SAW, full)));
    }
}

/* 5. A DIVISOR OF THE QUANTUM IS NOT THE QUANTUM, and neither real cell can
 *    tell them apart: at 28 and at 25 drawn columns, flooring to a multiple of
 *    12 and flooring to a multiple of 24 both land on 24. So halving the
 *    constant is invisible at every width that ships today and wrong at the
 *    first one that does not -- exactly the mutation that survived until this
 *    case was added. The width here is SYNTHETIC and says so. */
{
    const SYNTH = 44;                     /* 40 drawn: 24 under the real quantum, 36 under half */
    const s = drawnSpan(TRI, SYNTH);
    if (s.span % TRI_Q !== 0)
        bad("synthetic cell " + SYNTH + ": triangle drawn span " + s.span +
            " is not a multiple of the " + TRI_Q + "px quantum -- a divisor of it " +
            "(such as " + (TRI_Q / 2) + ") floors to the same width at both real cells " +
            "and diverges here");
    if (!uniform(runs(TRI, s.span)))
        bad("synthetic cell " + SYNTH + ": triangle at " + s.span + "px is not uniform, " +
            show(runs(TRI, s.span)));
}

if (fail === 0) {
    console.log("PASS: the triangle is drawn at a width its slope divides -- uniformly " +
        "stepped and centred at both cell sizes; sine, saw and square keep the full cell");
}
process.exit(fail ? 1 : 0);
'
