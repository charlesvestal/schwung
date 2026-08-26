#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A SECTION MARKER INSIDE THE DITHERED MASS IS A GAP, NOT A LINE.
#
# The markers were dottedV drawn over the CHECKER mass, and a dotted line over a
# 50% checker is never dotted: dottedV steps by 2 so all its pixels share one
# parity of y, and CHECKER lights (x+y)%2===0. The marker therefore either
# coincides with the mass and vanishes, or lands entirely in its gaps and the
# column comes out SOLID WHITE -- decided by the parity of (x + susY), which
# flips as a knob moves the boundary one pixel. Reported from the device as a
# white line where sections collide.
#
# THE ASSERTION IS PARITY-INVARIANCE, not a picture. That is the property the
# fix has and the bug did not, and it is the only thing that distinguishes them:
# at any SINGLE value the old marker looked fine half the time. So the envelope
# is swept and the invariant is required to hold at EVERY value.
#
# Two halves, and both are needed. A marker that vanished would satisfy "no
# solid column" trivially, so the knockout is also required to be VISIBLE on the
# four-role envelope -- a dark column with lit pixels on both sides.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the envelope marker test" >&2
  exit 1
fi

node --input-type=module -e '
import fs from "node:fs";
import { drawVizGroup, VIZ_ROWS } from "./src/shared/param_pages/viz_draw.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { resolveViz } from "./src/shared/param_pages/viz.mjs";
import { planPages } from "./src/shared/param_pages/page_plan.mjs";

let fail = 0;
const bad = (m) => { console.log("FAIL: " + m); fail++; };

const fleet = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8")).modules;

/* Every envelope group in the fleet, with its module and page. */
const envs = [];
for (const c of fleet) {
    let cp = c.chain_params;
    if (typeof cp === "string") { try { cp = JSON.parse(cp); } catch { continue; } }
    if (!Array.isArray(cp)) continue;
    let plan;
    try { plan = planPages({ hierarchy: c.ui_hierarchy, chainParams: cp }); } catch { continue; }
    const metaIndex = buildMetaIndex({ hierarchy: c.ui_hierarchy, chainParams: cp });
    for (const page of plan.pages || []) {
        if (!Array.isArray(page.keys)) continue;
        let groups = [];
        try { groups = resolveViz({ keys: page.keys, metaIndex }).groups || []; } catch { continue; }
        for (const g of groups)
            if (g.kind === "envelope") envs.push({ id: c.id, page, g, metaIndex });
    }
}
if (envs.length < 10) bad("only " + envs.length + " envelope groups found -- the sweep " +
    "is not covering the fleet and this test proves little");

const H = VIZ_ROWS + 2;
function render(e, f) {
    const W = Math.max(32, e.g.slotSpan * 32);
    const px = new Uint8Array(W * H);
    const ctx = { fillRect(x, y, w, h, c) {
        for (let j = y; j < y + h; j++)
            for (let i = x; i < x + w; i++)
                if (i >= 0 && i < W && j >= 0 && j < H) px[j * W + i] = c ? 1 : 0;
    } };
    const values = {};
    for (const k of e.page.keys) {
        if (!k) continue;
        const m = e.metaIndex.getOrGuess(k);
        const lo = Number.isFinite(m && m.min) ? m.min : 0;
        const hi = Number.isFinite(m && m.max) ? m.max : 1;
        values[k] = String(lo + f * (hi - lo));
    }
    drawVizGroup(ctx, { x: 0, y: 0, w: W, h: H }, e.g, values, e.metaIndex, null, undefined);
    return { px, W };
}
/*
 * Every judgement is made only on rows where BOTH neighbours are lit -- i.e.
 * where this column is genuinely INSIDE the mass.
 *
 * Without that, a fast attack is indistinguishable from the bug: at attack 0.05
 * the rise climbs 13 rows in about one column, which is a solid vertical run and
 * a perfectly correct one. It has open space beside it, and the bug does not.
 * The first version of this test flagged obxd at 0.05 for exactly that.
 *
 * The same row set answers the other half. A knockout is the same column dark
 * where the bug is lit, so both are measured against the same denominator
 * instead of against the cell height, which no column spans.
 */
function insideRows(r, x) {
    const rows = [];
    for (let y = 0; y < H; y++)
        if (r.px[y * r.W + (x - 1)] && r.px[y * r.W + (x + 1)]) rows.push(y);
    return rows;
}
function solidColumns(r) {
    const out = [];
    for (let x = 1; x < r.W - 1; x++) {
        const rows = insideRows(r, x);
        if (rows.length < 6) continue;                 /* not inside the mass */
        let lit = 0;
        for (const y of rows) if (r.px[y * r.W + x]) lit++;
        if (lit === rows.length) out.push(x);          /* solid through the mass */
    }
    return out;
}
/*
 * A KNOCKOUT CANNOT BE FOUND THE SAME WAY, and the reason is the parity trap
 * one level down.
 *
 * `insideRows` requires both neighbours lit. Neighbours sit at x-1 and x+1, so
 * they share a parity with each other and the OPPOSITE one to x -- which means
 * on exactly those rows CHECKER never lights x. Every ordinary column in the
 * mass therefore looks like a knockout, and the first version of this check
 * passed with the markers deleted outright.
 *
 * So a knockout is measured against what CHECKER WOULD have drawn: take the
 * vertical extent of the mass from the neighbours, and require every row where
 * the checker would light this column to be dark instead.
 */
function knockoutColumns(r) {
    const out = [];
    for (let x = 1; x < r.W - 1; x++) {
        let lo = -1, hi = -1;
        for (let y = 0; y < H; y++)
            if (r.px[y * r.W + (x - 1)] || r.px[y * r.W + (x + 1)]) { if (lo < 0) lo = y; hi = y; }
        if (lo < 0) continue;
        /* Strictly INSIDE: the curve stroke bounds the mass above and the floor
         * line below, and both are meant to survive a knockout through the
         * middle -- the marker divides the fill, it does not cut the shape in
         * half. Including either endpoint makes lit === 0 unsatisfiable. */
        let expected = 0, lit = 0;
        for (let y = lo + 1; y <= hi - 1; y++) {
            if (((x + y) % 2) !== 0) continue;         /* CHECKER would not light it */
            expected++;
            if (r.px[y * r.W + x]) lit++;
        }
        if (expected >= 3 && lit === 0) out.push(x);
    }
    return out;
}

const SWEEP = [0.05, 0.17, 0.29, 0.41, 0.5, 0.62, 0.74, 0.86, 0.97];
let checked = 0, worst = null;
for (const e of envs) {
    for (const f of SWEEP) {
        const r = render(e, f);
        checked++;
        const cols = solidColumns(r);
        if (cols.length && !worst) worst = e.id + " at " + f + ", columns " + cols.join(",");
    }
}
if (worst) bad("a solid vertical rule appears inside the mass: " + worst +
    " -- a marker drawn ADDITIVELY over CHECKER lands in its gaps and fills the column");
console.log("  swept " + checked + " renders across " + envs.length + " envelope groups");

/* The other half: on a four-role envelope the knockout must be VISIBLE. */
{
    const full = envs.find((e) => e.g.roles.attack && e.g.roles.decay
                                  && e.g.roles.sustain && e.g.roles.release);
    if (!full) {
        bad("no four-role envelope in the fixture -- cannot check the marker is visible");
    } else {
        const r = render(full, 0.5);
        const gapCols = knockoutColumns(r).length;
        if (gapCols < 1)
            bad("no knockout column on " + full.id + " -- the marker is invisible, which " +
                "satisfies the no-solid-rule assertion trivially and marks nothing");
    }
}

if (fail === 0) {
    console.log("PASS: no envelope draws a solid rule inside its dithered mass at any value, " +
        "and the four-role markers still read as gaps");
}
process.exit(fail ? 1 : 0);
'
