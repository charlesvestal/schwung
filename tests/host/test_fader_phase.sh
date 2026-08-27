#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A DETENT MUST MOVE SOMETHING.
#
# The fader bar is 7px wide in a 13-row band, so a 128-step parameter gets about
# ten detents per row and nine in ten used to change no pixel at all: 12 of 127.
# Phasing the interior lattice by the sub-row remainder gives four phases between
# one row and the next, taking it to 44 of 127.
#
# THE ASSERTION IS THAT COUNT, NOT A PICTURE. A pixel baseline passes with the
# phase computed from anything at all -- including from a constant, which would
# restore the exact bug this exists to fix while still drawing a plausible
# fader. What is being defended is feedback per detent, so that is what is
# measured.
#
# BAND BY BAND, NOT JUST THE TOTAL. A total hides where the dead detents are,
# and during this work a variant scored 37/127 overall while its LOW third had
# collapsed from 38% to 17% -- the third where a level is nudged rather than
# grabbed. The total barely moved and would not have caught it. Each third
# therefore carries its own floor.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the fader phase test" >&2
  exit 1
fi

node --input-type=module -e '
import { drawFader } from "./src/shared/param_pages/viz_draw.mjs";
import { CELL_W } from "./src/shared/param_pages/render_page_movy.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const W = CELL_W, H = 15, STEPS = 128;
const meta = { getOrGuess: () => ({ type: "int", kind: "number", min: 0, max: STEPS - 1 }) };

function frame(v) {
    const px = new Uint8Array(W * H);
    const ctx = { fillRect(x, y, w, h, c) {
        for (let j = y; j < y + h; j++)
            for (let i = x; i < x + w; i++)
                if (i >= 0 && i < W && j >= 0 && j < H) px[j * W + i] = c ? 1 : 0;
    } };
    drawFader(ctx, { x: 0, y: 0, w: W, h: H }, "v", { v: String(v) }, meta);
    return Buffer.from(px).toString("base64");
}
const frames = [];
for (let v = 0; v < STEPS; v++) frames.push(frame(v));

const moved = (a, b) => {
    let n = 0;
    for (let i = Math.max(a, 1); i < b; i++) if (frames[i] !== frames[i - 1]) n++;
    return n;
};

/* Floors, not exact counts: the point is that feedback EXISTS in each third,
 * and an exact number would fail on any unrelated geometry tweak while telling
 * nobody anything. Unphased, these thirds score 10 / 10 / 12 percent, so a
 * floor of 20 is comfortably below the phased result and comfortably above the
 * bug. */
const FLOOR_PCT = 20;
const bands = [["low", 1, 43], ["mid", 43, 86], ["high", 86, 128]];
for (const [name, a, b] of bands) {
    const n = moved(a, b), total = b - Math.max(a, 1);
    const pct = Math.round(100 * n / total);
    if (pct < FLOOR_PCT)
        bad(name + " third: only " + n + "/" + total + " detents (" + pct + "%) change a " +
            "pixel, floor is " + FLOOR_PCT + "% -- the sub-row phase is not reaching this " +
            "part of the travel");
}

/* And the whole sweep, which is what regressed to 12/127 before. */
const total = moved(1, STEPS);
if (total < 30)
    bad("only " + total + "/127 detents change a pixel over the full sweep -- unphased " +
        "this is 12, so the lattice phase is not being applied at all");

/* The phase must come from the VALUE. A constant phase draws a perfectly
 * plausible fader and silently restores the bug, so it is pinned directly:
 * two values inside the SAME row must differ. */
{
    const h = 12;                       /* band rows - 1, the fader travel */
    const perRow = (STEPS - 1) / h;     /* detents per row, about 10.6 */
    const v0 = 60, v1 = Math.round(v0 + perRow / 2);
    if (frames[v0] === frames[v1])
        bad("values " + v0 + " and " + v1 + " sit within one row of travel and render " +
            "identically -- the phase is not a function of the value");
}

if (fail === 0) {
    console.log("PASS: every third of the fader travel gives feedback on at least " +
        FLOOR_PCT + "% of detents (" + total + "/127 overall), and the lattice phase " +
        "tracks the value within a row");
}
process.exit(fail ? 1 : 0);
'
