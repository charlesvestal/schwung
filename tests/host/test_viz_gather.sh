#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A PICTURE GETS THE WIDTH ITS CONTROLS WARRANT -- AND THEN STOPS PEEKING.
#
# Two rules that only make sense together.
#
# GATHER. A graphic must be CONTIGUOUS, so a module that writes its members
# apart gets one 30px cell no matter how many controls feed the picture.
# granny is the case: `position` on knob 1, `spray` on knob 4, and the fences
# spray controls drawn onto a cell belonging to position. Seating them together
# is a reorder behind the author back, so it carries the same guarantees
# alignGroupsToRows does -- same keys, one row, verified by the real detector.
#
# NO PEEK WHEN WIDE. The peek exists because a 30px cell cannot show a list or
# a waveform. Once the graphic has the room, covering the page with a panel
# hides the rest of the row to show nothing new.
#
# THE NARROWNESS IS THE FEATURE. Measured over the fleet fixture, three pages
# move and 486 do not. A pass that re-seated every page would be a layout
# engine, which is a different and much larger decision.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the gather tests" >&2
  exit 1
fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { gatherGroupMembers, resolveViz, VIZ_SAMPLE }
  from "./src/shared/param_pages/viz.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const idx = (chainParams, keys) => buildMetaIndex({
  hierarchy: { modes: null, levels: { root: {
    label: "T", knobs: keys.filter(Boolean),
    params: chainParams.map((p) => ({ key: p.key })) } } },
  chainParams,
});
const sampleOf = (keys, mi) =>
  (resolveViz({ keys, metaIndex: mi }).groups || []).find((g) => g.kind === VIZ_SAMPLE);

/* ===================================================================== 1 ==
 * THE GRANNY SHAPE: cursor at knob 1, spray at knob 4, two unrelated knobs
 * between them.
 */
{
  const cp = [
    { key: "position", name: "Position", type: "wav_position", min: 0, max: 1 },
    { key: "size_ms",  name: "Size",     type: "float", min: 0, max: 1 },
    { key: "density",  name: "Density",  type: "float", min: 0, max: 1 },
    { key: "spray",    name: "Spray",    type: "float", min: 0, max: 1 },
    { key: "jitter",   name: "Jitter",   type: "float", min: 0, max: 1 },
  ];
  const keys = ["position", "size_ms", "density", "spray", "jitter", null, null, null];
  const mi = idx(cp, keys);

  ok(sampleOf(keys, mi).slotSpan === 1, "before: the graphic is one cell");

  const r = gatherGroupMembers(keys, mi);
  ok(r.moved, "the pass fires");
  ok(r.span === 2, "and the graphic wants two cells, got " + r.span);

  const g = sampleOf(r.keys, mi);
  ok(g && g.slotSpan === 2, "the REAL detector agrees it is two cells wide");
  ok(g && g.keys.indexOf("position") >= 0 && g.keys.indexOf("spray") >= 0,
     "and both members are cells of it");

  /* THE GUARANTEE THAT MAKES A REORDER SAFE. */
  const sortJoin = (a) => a.filter(Boolean).slice().sort().join(",");
  ok(sortJoin(r.keys) === sortJoin(keys),
     "WHICH keys are on the page is unchanged -- no knob is pushed to another "
     + "page and no orphan page appears");
  ok(r.keys.length === keys.length, "the slot count is unchanged");
  ok(r.keys[0] === "position" && r.keys[1] === "spray",
     "the members are seated together, in order, got "
     + JSON.stringify(r.keys.slice(0, 2)));
  ok(r.keys.indexOf("size_ms") === 2 && r.keys.indexOf("density") === 3,
     "the displaced knobs keep their relative order");
}

/* ===================================================================== 2 ==
 * NOTHING SCATTERED, NOTHING MOVES. The common case by a wide margin, and the
 * one where a reorder would be pure damage.
 */
{
  const cp = [
    { key: "sample_path", name: "Sample",   type: "filepath" },
    { key: "position",    name: "Position", type: "wav_position", min: 0, max: 1 },
    { key: "gain",        name: "Gain",     type: "float", min: 0, max: 1 },
  ];
  const keys = ["sample_path", "position", "gain", null, null, null, null, null];
  const mi = idx(cp, keys);
  const r = gatherGroupMembers(keys, mi);
  ok(!r.moved, "a group whose members are already together is left alone");
  ok(r.keys === keys, "and the identical array is handed straight back");
}

/* ===================================================================== 3 ==
 * ONE ROW, ALWAYS. A shape spanning the row break draws straight through the
 * label band -- row 0 labels sit at y=25..32 and row 1 starts at y=33. This is
 * a hard constraint, not a preference, so the pass must decline rather than
 * produce a graphic that renders over text.
 */
{
  const cp = [
    { key: "a", name: "A", type: "float", min: 0, max: 1 },
    { key: "b", name: "B", type: "float", min: 0, max: 1 },
    { key: "c", name: "C", type: "float", min: 0, max: 1 },
    { key: "position", name: "Position", type: "wav_position", min: 0, max: 1 },
    { key: "d", name: "D", type: "float", min: 0, max: 1 },
    { key: "e", name: "E", type: "float", min: 0, max: 1 },
    { key: "f", name: "F", type: "float", min: 0, max: 1 },
    { key: "spray", name: "Spray", type: "float", min: 0, max: 1 },
  ];
  /* position lands on slot 3 -- the last cell of row one -- so seating spray
     after it would put the pair across the break. */
  const keys = ["a", "b", "c", "position", "d", "e", "f", "spray"];
  const mi = idx(cp, keys);
  const r = gatherGroupMembers(keys, mi);
  const g = sampleOf(r.keys, mi);
  const start = r.keys.indexOf("position");
  const straddles = g && g.slotSpan > 1 &&
                    Math.floor(start / 4) !== Math.floor((start + g.slotSpan - 1) / 4);
  ok(!straddles, "the pass never leaves a graphic across the row break");
}

/* ===================================================================== 4 ==
 * THE FLEET. The claim is that this is a nudge, not a layout engine -- so
 * count it. A regression that starts re-seating pages shows up here as a
 * number, long before anyone notices a knob moved on a synth they do not use.
 */
{
  const db = JSON.parse(readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  let pages = 0, moved = 0;
  const movedList = [];
  for (const m of db.modules) {
    let cps = m.chain_params;
    if (typeof cps === "string") { try { cps = JSON.parse(cps); } catch (e) { continue; } }
    if (!Array.isArray(cps)) continue;
    let h = m.ui_hierarchy;
    if (typeof h === "string") { try { h = JSON.parse(h); } catch (e) { h = null; } }
    if (!h || !h.levels) continue;
    const mi = buildMetaIndex({ chainParams: cps, hierarchy: h });
    for (const [lvl, v] of Object.entries(h.levels)) {
      if (!v.knobs) continue;
      for (let pg = 0; pg * 8 < v.knobs.length; pg++) {
        const keys = v.knobs.slice(pg * 8, pg * 8 + 8);
        while (keys.length < 8) keys.push(null);
        pages++;
        const r = gatherGroupMembers(keys, mi);
        if (!r.moved) continue;
        moved++;
        movedList.push(m.id + "/" + lvl + ":" + r.span);
      }
    }
  }
  console.log("       (" + moved + " of " + pages + " pages: " + movedList.join(" ") + ")");
  ok(pages > 400, "the fleet fixture really was walked, got " + pages + " knob pages");
  ok(moved === 3, "exactly three pages are re-seated, got " + moved);
  ok(movedList.indexOf("granny/root:2") >= 0, "granny root widens to two cells");
  ok(movedList.indexOf("granny/main:2") >= 0, "granny main widens to two cells");
  ok(movedList.indexOf("mrsample/sample:3") >= 0,
     "mrsample loop page widens to three cells");
}

process.exit(fail ? 1 : 0);
'
