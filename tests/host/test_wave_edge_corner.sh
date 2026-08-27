#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A HARD EDGE KEEPS ITS CORNER; A SHALLOW RAMP MUST NOT.
#
# The stroke draws only the rows a column NEWLY occupies, which is what makes a
# ramp a 1px staircase rather than a two-column zigzag. At a 1-row step the
# omitted pixel IS the staircase. At the square's 12-row step it was a nick out
# of a vertical edge -- the falling edge began one row below the rail it falls
# from, so the rail looked like it stopped a pixel early. Reported from the
# device as the square missing a pixel at its first turn.
#
# BOTH HALVES ARE ASSERTED, because either alone is satisfied by a broken rule:
# "the square corner is filled" passes if the threshold is 1 (which brings the
# zigzag back on every shape), and "the triangle has no doubled column" passes
# if the corner rule is deleted entirely.
#
# THE SQUARE HALF IS PHRASED AS 4-CONNECTIVITY, not as a pixel coordinate. The
# corner pixel is at whatever column the edge happens to land on, and hard-coding
# it would re-break silently the moment a width or a knee moved. What is actually
# required is that the rail and the riser touch orthogonally.
#
# MEASURED ON THE ODD PARITY ONLY. The stroke shares its yAt closure with the
# CHECKER mass fill, and CHECKER lights (x+y)%2===0 -- so half of every filled
# region is lit and a naive connectivity walk wanders off through the dither.
# Odd-parity ink is stroke, by construction.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the wave edge test" >&2
  exit 1
fi

node --input-type=module -e '
import { drawWaveform } from "./src/shared/param_pages/viz_draw.mjs";
import { CELL_W } from "./src/shared/param_pages/render_page_movy.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const H = 15;
const metaIndex = { getOrGuess: () => ({ type: "enum", kind: "enum",
    options: ["sine", "tri", "saw", "square"] }) };

function render(shapeIndex, W) {
    const px = new Uint8Array(W * H);
    const ctx = { fillRect(x, y, w, h, c) {
        for (let j = y; j < y + h; j++)
            for (let i = x; i < x + w; i++)
                if (i >= 0 && i < W && j >= 0 && j < H) px[j * W + i] = c ? 1 : 0;
    } };
    drawWaveform(ctx, { x: 0, y: 0, w: W, h: H }, "s",
        { s: String(shapeIndex) }, metaIndex, null, undefined);
    return { px, W };
}
/* Stroke ink only: CHECKER lights even parity, so odd parity is unambiguous. */
const strokeAt = (f, x, y) =>
    x >= 0 && x < f.W && y >= 0 && y < H && f.px[y * f.W + x] === 1 && ((x + y) % 2) === 1;
const litAt = (f, x, y) => x >= 0 && x < f.W && y >= 0 && y < H && f.px[y * f.W + x] === 1;

/* 1. THE SQUARE. Find its top rail, then require the column just past the rail
 *    to be lit at the rail row -- that is the corner, and it makes the rail and
 *    the riser 4-connected instead of merely diagonal. */
{
    const f = render(3, CELL_W);
    /* The rail is the longest run of lit pixels on any single row. */
    let best = { len: 0 };
    for (let y = 0; y < H; y++) {
        let run = 0;
        for (let x = 0; x <= f.W; x++) {
            if (x < f.W && litAt(f, x, y)) run++;
            else { if (run > best.len) best = { len: run, y, end: x - 1 }; run = 0; }
        }
    }
    if (best.len < 6) {
        bad("could not find the squares top rail (longest run was " + best.len + "px)");
    } else {
        /* The rail and the riser must SHARE a column. When the corner is drawn
         * the rail runs one further, into the riser column, so the join is that
         * column carrying both the rail row and the full drop. Chamfered, the
         * rail stops one short and its last column holds a single pixel. */
        const cx = best.end;
        let drop = 0;
        for (let y = 0; y < H; y++) if (litAt(f, cx, y)) drop++;
        if (drop < 6)
            bad("the square is chamfered: the rail on row " + best.y + " ends at x=" + cx +
                ", where only " + drop + " row(s) are lit -- the riser starts in the NEXT " +
                "column and one row lower, so rail and riser meet only diagonally");
    }
}

/* 2. THE RAMPS. No column may carry two stroke rows from a 1-row step -- that
 *    is the chunky zigzag, and a threshold of 1 reintroduces it on every shape.
 *
 *    Parity cannot be used to identify the stroke here, which is the trap: two
 *    ADJACENT rows have opposite parity, so a doubled column contributes
 *    exactly one odd-parity pixel, the same as a single one. Counting odd
 *    parity per column therefore reports the zigzag as clean, and did.
 *
 *    The exact test uses parity the other way round. Take the topmost lit row
 *    r in a column on the upper half (where the CHECKER mass lies BELOW the
 *    curve, so the topmost lit pixel is stroke). If the checker would NOT light
 *    r+1, then r+1 being lit can only be a second stroke row. Those columns are
 *    a decisive sample rather than a statistical one.
 *
 *    THE TRIANGLE CANNOT BE THE SUBJECT, for a reason that is invisible until
 *    the pixels are printed: quantized, its ramp is exactly one row per column,
 *    so x+r is CONSTANT along the whole limb and every column lands on the same
 *    side of the parity test. The decidable set is empty by construction. The
 *    saw and the sine have non-unit slopes, so parity alternates along them and
 *    it can carry the assertion -- and a threshold of 1 doubles its columns
 *    too, so nothing is lost by testing there. The `checked` floor below is
 *    what turned that into a failure instead of a silent pass.
 *
 *    NOR THE SINE, for the opposite reason: it genuinely contains three 2-row
 *    steps at this width, so the corner rule fires on it BY DESIGN and a
 *    doubled column is correct there. The saw is the only shape that is both
 *    all-1-row and non-unit slope, which is what this assertion needs. */
const AXIS = (H - 1) / 2;
for (const [shape, name] of [[2, "saw"]]) {
    const f = render(shape, CELL_W);
    let doubled = 0, checked = 0;
    for (let x = 0; x < f.W; x++) {
        let r = -1;
        for (let y = 0; y < H; y++) if (litAt(f, x, y)) { r = y; break; }
        if (r < 0 || r >= AXIS) continue;                 /* upper half only */
        if (((x + r + 1) % 2) === 0) continue;            /* checker lights it: ambiguous */
        checked++;
        if (litAt(f, x, r + 1)) doubled++;
    }
    if (checked < 4)
        bad(name + " gave only " + checked + " decidable column(s) -- too few to " +
            "conclude anything, so this assertion is not doing its job");
    else if (doubled > 0)
        bad(name + " has " + doubled + " of " + checked + " column(s) carrying a " +
            "doubled stroke -- the corner rule is firing on a 1-row step and the " +
            "ramp is a zigzag again");
}

if (fail === 0) {
    console.log("PASS: a hard edge keeps its corner (square rail and riser are 4-connected); " +
        "1-row ramps stay a single-pixel staircase");
}
process.exit(fail ? 1 : 0);
'
