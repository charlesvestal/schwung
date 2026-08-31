#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# An A/D/R envelope must not be drawn as a flat line along the floor (#359).
#
# "No sustain role" was read as "sustain is zero". That is right for A/D -- a
# percussive shape decays to silence and there is nothing to release -- and
# wrong the moment `release` is declared, because release means "fall from the
# held level to zero". If the held level were already zero there would be
# nothing to release from. So the presence of `release` is itself the evidence
# that a sustain STAGE exists, whether or not its LEVEL is a parameter.
#
# Reported against a Yamaha QY-70 editor, whose XG Multi Part offsets are EG
# Attack, EG Decay and EG Release and nothing else: the sustain level lives in
# the voice and is not addressable, but hold a note and it holds.
#
# SYNTHETIC GROUPS, deliberately. The 100-module fleet capture contains ZERO
# A/D/R-without-sustain envelopes -- checked -- so there is no fixture to drive
# this from, and a test that quietly covered nothing would be worse than none.
# The shape is copied from a real detected group (slicer "Params - 2").
#
# Both halves are needed. Asserting only "A/D/R is off the floor" passes with
# every envelope pinned to the ceiling, so A/D is required to still REACH the
# floor -- that case was correct before and must stay correct.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { drawVizGroup, VIZ_ROWS } from "./src/shared/param_pages/viz_draw.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };
const ok  = (m) => console.log("  ok  " + m);

/* A contract whose keys carry the envelope roles by name. */
const chainParams = [
    { key: "eg_attack",  name: "Attack",  type: "float", min: 0, max: 1, step: 0.01 },
    { key: "eg_decay",   name: "Decay",   type: "float", min: 0, max: 1, step: 0.01 },
    { key: "eg_sustain", name: "Sustain", type: "float", min: 0, max: 1, step: 0.01 },
    { key: "eg_release", name: "Release", type: "float", min: 0, max: 1, step: 0.01 },
];
const metaIndex = buildMetaIndex({ chainParams });

const H = VIZ_ROWS + 2;
function render(roles, vals) {
    const keys = Object.values(roles);
    const g = { kind: "envelope", group: null, roles, keys,
                slotStart: 0, slotSpan: keys.length, source: "detected" };
    const W = Math.max(32, g.slotSpan * 32);
    const px = new Uint8Array(W * H);
    const ctx = { fillRect(x, y, w, h, c) {
        for (let j = y; j < y + h; j++)
            for (let i = x; i < x + w; i++)
                if (i >= 0 && i < W && j >= 0 && j < H) px[j * W + i] = c ? 1 : 0;
    } };
    const values = {};
    for (const k of keys) values[k] = String(vals[k] !== undefined ? vals[k] : 0.5);
    drawVizGroup(ctx, { x: 0, y: 0, w: W, h: H }, g, values, metaIndex, null, undefined);
    return { px, W };
}

/* The topmost lit row in each column: the outline of the curve. Rows count
 * DOWN, so a smaller number is higher on screen. */
function topRow(r, x) {
    for (let y = 0; y < H; y++) if (r.px[y * r.W + x]) return y;
    return null;
}
/* How high the curve stands, in rows above the lowest row it ever reaches. */
function profile(r) {
    const tops = [];
    for (let x = 0; x < r.W; x++) tops.push(topRow(r, x));
    return tops;
}

const ADR = { attack: "eg_attack", decay: "eg_decay", release: "eg_release" };
const AD  = { attack: "eg_attack", decay: "eg_decay" };
const ADSR = { attack: "eg_attack", decay: "eg_decay",
               sustain: "eg_sustain", release: "eg_release" };

/* ---- the reported case ------------------------------------------------- */
{
    const r = render(ADR, { eg_attack: 0.3, eg_decay: 0.4, eg_release: 0.5 });
    const tops = profile(r);
    const floor = Math.max(...tops.filter((v) => v !== null));

    /* The right-hand HALF of the picture is where decay has finished and the
     * release runs. Under the bug every column there sits on the floor. */
    const rightHalf = [];
    for (let x = Math.floor(r.W / 2); x < r.W; x++) if (tops[x] !== null) rightHalf.push(tops[x]);
    const aboveFloor = rightHalf.filter((v) => v < floor - 1).length;

    if (aboveFloor === 0)
        bad("an A/D/R envelope is still flat along the floor after decay -- " +
            "there is no nonzero portion that reads as sustained");
    else
        ok("an A/D/R envelope holds above the floor before releasing (" +
           aboveFloor + " columns)");

    /* And it must still COME DOWN -- an implied sustain that never releases
     * would be its own wrong picture. */
    if (tops[r.W - 1] === null || tops[r.W - 1] < floor - 1)
        bad("the A/D/R release never reaches the floor");
    else
        ok("the A/D/R release still falls to the floor");
}

/* ---- the case that was already right ----------------------------------- */
{
    const r = render(AD, { eg_attack: 0.3, eg_decay: 0.4 });
    const tops = profile(r);
    const floor = Math.max(...tops.filter((v) => v !== null));
    /* A/D has nothing to release: it decays to silence and must REACH bottom. */
    const last = tops[r.W - 1];
    if (last !== null && last < floor - 1)
        bad("an A/D envelope no longer decays to the floor -- the implied " +
            "sustain leaked into the case that has no release");
    else
        ok("an A/D envelope still decays to the floor");
}

/* ---- a declared sustain still wins ------------------------------------- */
{
    const lo = render(ADSR, { eg_attack: 0.3, eg_decay: 0.4, eg_sustain: 0.05, eg_release: 0.5 });
    const hi = render(ADSR, { eg_attack: 0.3, eg_decay: 0.4, eg_sustain: 0.95, eg_release: 0.5 });
    const mid = Math.floor(lo.W * 0.55);
    const loTop = topRow(lo, mid), hiTop = topRow(hi, mid);
    if (loTop === null || hiTop === null || !(hiTop < loTop))
        bad("a DECLARED sustain no longer sets the plateau height (low=" +
            loTop + " high=" + hiTop + ") -- the implied value overrode a real one");
    else
        ok("a declared sustain value still sets the plateau height");
}

if (fail) { console.log("FAILURES: " + fail); process.exit(1); }
console.log("PASS: a release role implies a sustain stage to draw");
'
