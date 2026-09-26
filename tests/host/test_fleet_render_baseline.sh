#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# WHAT THE FLEET DRAWS AND TURNS TODAY, PINNED.
#
# The opt-in declarations (turn: absolute, display: big) and host hooks
# (io.enumPeek, io.feel) are all claims about modules that declare NOTHING.
# That claim is cheap to make and easy to break by accident -- a predicate
# widened one clause too far takes the whole fleet with it -- so it is measured
# rather than asserted: for every cell of every planned page of every module in
# the fixture, the widget kind it draws and where one scripted gesture lands it
# (four clockwise detents then four counter-clockwise, spaced past the two-way
# latch), which is what catches a two-way that stopped toggling.
#
# Regenerate ONLY when a change is meant to move the fleet:
#   UPDATE_BASELINE=1 bash tests/host/test_fleet_render_baseline.sh
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the fleet render baseline" >&2
  exit 1
fi

node --input-type=module -e '
import fs from "node:fs";
import { widgetKindFor } from "./src/shared/param_pages/render_page_movy.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { planPages } from "./src/shared/param_pages/page_plan.mjs";
import { knobInit, knobStep } from "./src/shared/knob_engine.mjs";

function gesture(m) {
    const st = knobInit(typeof m.min === "number" && isFinite(m.min) ? m.min : 0);
    const seen = [];
    let t = 1000;
    for (const d of [1, 1, 1, 1, -1, -1, -1, -1]) { seen.push(knobStep(st, m, d, t)); t += 1000; }
    return seen.map((v) => (typeof v === "number" ? +v.toFixed(4) : String(v))).join(",");
}

const fleet = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8")).modules;
const out = {};
let cells = 0;
for (const c of fleet) {
    let cp = c.chain_params;
    if (typeof cp === "string") { try { cp = JSON.parse(cp); } catch { continue; } }
    if (!Array.isArray(cp)) continue;
    let plan;
    try { plan = planPages({ hierarchy: c.ui_hierarchy, chainParams: cp }); } catch { continue; }
    const metaIndex = buildMetaIndex({ hierarchy: c.ui_hierarchy, chainParams: cp });
    const rows = [];
    for (const page of plan.pages || []) {
        for (const k of page.keys || []) {
            if (!k) continue;
            const m = metaIndex.getOrGuess(k);
            if (!m) continue;
            rows.push(k + " " + widgetKindFor(m) + " " + gesture(m));
            cells++;
        }
    }
    out[c.id] = rows;
}
const path = "tests/fixtures/fleet-render-baseline.json";
const text = JSON.stringify(out, null, 1) + "\n";
const n = Object.keys(out).length;
if (process.env.UPDATE_BASELINE === "1") {
    fs.writeFileSync(path, text);
    console.log("PASS: baseline written, " + n + " modules, " + cells + " cells");
} else if (!fs.existsSync(path)) {
    console.log("FAIL: no baseline; run once with UPDATE_BASELINE=1");
    process.exit(1);
} else {
    const was = JSON.parse(fs.readFileSync(path, "utf8"));
    const diffs = [];
    for (const id of new Set([...Object.keys(was), ...Object.keys(out)])) {
        const a = was[id] || [], b = out[id] || [];
        for (let i = 0; i < Math.max(a.length, b.length); i++)
            if (a[i] !== b[i]) diffs.push(id + ": " + (a[i] ?? "(none)") + "  ->  " + (b[i] ?? "(none)"));
    }
    if (diffs.length) {
        console.log("FAIL: the fleet draws or turns differently than the baseline records (" +
                    diffs.length + " cells), e.g.");
        for (const d of diffs.slice(0, 6)) console.log("  " + d);
        process.exit(1);
    }
    if (cells < 500) { console.log("FAIL: only " + cells + " cells swept -- not reaching the fleet"); process.exit(1); }
    console.log("PASS: fleet unchanged, " + n + " modules, " + cells + " cells");
}
'
