#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A SEQUENCER LANE AND A MODULATION SOURCE WEAR DIFFERENT MARKS.
#
# Both move a parameter, and the controller treats them alike for the MOTION --
# pointer on `:base`, the dot riding `:effective`. A host used to get that by
# answering yes to `isModulated` for a lane, which drew the modulation tilde on
# an automated cell: an LFO and a lane looked the same, and a parameter under
# both said it once. `io.isAutomated` keeps the motion and gives the lane its
# own mark, a 2x2 at the top-right of the label.
#
# Pixels, not a grep, for the marks; reads, not a grep, for the motion -- the
# point of the split is that the two halves stop being one flag, so each half
# is asserted where it actually happens.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_MOVY }
  from "./src/shared/param_pages/page_controller.mjs";
import { drawLabelCell, drawAutomatedMark }
  from "./src/shared/param_pages/render_page_movy.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";

let fails = 0;
const check = (c, m) => { if (!c) { console.log("FAIL: " + m); fails++; } };

/* ---- 1. the mark, in the label band ----------------------------------- */
const G = { x0: 0, cellW: 32 };
const LBL = 20;
function band(showValue, modulated, automated, label = "CUT", value = "0.42") {
  const fb = createFramebuffer();
  drawLabelCell(drawContext(fb), G, 0, LBL, label, value, showValue, showValue,
                modulated, automated);
  return fb;
}
/* The pixels that differ between two drawings, as [x, y, colourInB]. */
function diff(a, b) {
  const out = [];
  for (let i = 0; i < a.pixels.length; i++)
    if (a.pixels[i] !== b.pixels[i]) out.push([i % a.width, (i / a.width) | 0, b.pixels[i]]);
  return out;
}
const isBlock = (d, on) => {
  if (d.length !== 4) return false;
  const xs = d.map((p) => p[0]), ys = d.map((p) => p[1]);
  const x0 = Math.min(...xs), y0 = Math.min(...ys);
  return d.every(([x, y, c]) => x - x0 < 2 && y - y0 < 2 && c === on);
};
const rightmostLit = (fb, rows) => {
  let r = -1;
  for (const y of rows) for (let x = 0; x < fb.width; x++) if (fb.pixels[y * fb.width + x]) r = Math.max(r, x);
  return r;
};

check(typeof drawAutomatedMark === "function",
      "drawAutomatedMark is not exported -- a host cannot tell this library draws the mark");

{
  /* At rest: a lit 2x2 on the band top row, right of the name. */
  const plain = band(false, false, false), auto = band(false, false, true);
  const d = diff(plain, auto);
  check(isBlock(d, 1), "an automated cell at rest must add exactly one lit 2x2, got " + JSON.stringify(d));
  if (d.length) {
    const x = Math.min(...d.map((p) => p[0])), y = Math.min(...d.map((p) => p[1]));
    check(y === LBL, "the mark must sit on the band top row " + LBL + ", got " + y);
    check(x > rightmostLit(plain, [LBL + 1, LBL + 2, LBL + 3, LBL + 4, LBL + 5]),
          "the mark must sit PAST the text, not on it (x=" + x + ")");
  }
  /* And it is NOT the tilde: the two marks differ, and a cell with both draws both. */
  const mod = band(false, true, false), both = band(false, true, true);
  check(diff(auto, mod).length > 0, "automated and modulated drew the same mark");
  check(isBlock(diff(mod, both), 1), "modulated + automated must be the tilde plus the 2x2");
}
{
  /* Held / locked: the strip carries the value, the mark stays lit beside it. */
  const held = band(true, false, false), heldAuto = band(true, false, true);
  const d = diff(held, heldAuto);
  check(isBlock(d, 1), "on an inverted strip the mark must still be a lit 2x2 on ground beside it");
  /* ...with a clear column between: touching, it reads as a notch in the strip. */
  if (d.length) {
    const x = Math.min(...d.map((p) => p[0]));
    const at = (xx, yy) => held.pixels[yy * held.width + xx];
    check(!at(x - 1, LBL + 2) && !at(x - 1, LBL + 3),
          "the mark must not touch the strip (x=" + x + ")");
  }
}
{
  /* A run that fills the cell pushes the clamp back over the strip: dark mark. */
  const held = band(true, false, false, "X", "-24.00"), heldAuto = band(true, false, true, "X", "-24.00");
  const d = diff(held, heldAuto);
  check(d.length > 0 && d.every((p) => p[0] < 32), "the mark must stay inside its cell");
}
{
  /* Absent argument = the old drawing, byte for byte. */
  const fbA = createFramebuffer(), fbB = createFramebuffer();
  drawLabelCell(drawContext(fbA), G, 0, LBL, "CUT", "0.42", false, false, true);
  drawLabelCell(drawContext(fbB), G, 0, LBL, "CUT", "0.42", false, false, true, false);
  check(diff(fbA, fbB).length === 0, "automated=false must be the pre-existing drawing");
}

/* ---- 2. the controller: motion from either flag, marks from each ----- */
const CP = [
  { key: "cutoff", name: "Cutoff", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "res",    name: "Res",    type: "float", min: 0, max: 1, step: 0.01 },
];
const HIER = { modes: null, levels: { root: { label: "T",
  knobs: ["cutoff", "res"], params: CP.map((p) => ({ key: p.key })) } } };
function rig(io) {
  const reads = [];
  const store = { cutoff: "0.9", "cutoff:base": "0.2", "cutoff:effective": "0.9", res: "0.5" };
  const ctl = createController(Object.assign({
    getParam: (k) => {
      reads.push(k);
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(HIER);
      if (b === "chain_params") return JSON.stringify(CP);
      return b in store ? store[b] : "";
    },
    setParam: () => {}, announce: () => {},
  }, io));
  ctl.setLayout(LAYOUT_MOVY);
  ctl.load({ prefix: "synth" });
  for (let i = 0; i < 40; i++) ctl.tick();
  const fb = createFramebuffer();
  ctl.render(drawContext(fb));
  return { ctl, reads, fb };
}
const none = rig({ isModulated: () => false });
const lane = rig({ isModulated: () => false, isAutomated: (k) => k === "synth:cutoff" });
const lfo  = rig({ isModulated: (k) => k === "synth:cutoff" });

check(lane.reads.includes("synth:cutoff:base"),
      "an automated key must read its pointer from :base, as a modulated one does");
check(lane.reads.includes("synth:cutoff:effective"),
      "an automated key must read its riding dot from :effective");
check(lane.ctl.state.values.cutoff === "0.2", "pointer should be the base, got " + lane.ctl.state.values.cutoff);
check(lane.ctl.state.modValues.cutoff === "0.9", "the riding dot should be the lane value");
check(lane.ctl.isAutomatedCached("cutoff") && !lane.ctl.isModulatedCached("cutoff"),
      "the lane must be cached as automated and NOT as modulated");
check(!lane.reads.includes("synth:res:base"), "a key nobody drives must not pay the :base read");
/* Same motion, different mark: the knob pixels agree, the band does not. */
check(diff(lane.fb, lfo.fb).length > 0, "a lane and an LFO rendered identically -- one mark for two causes");
check(diff(lane.fb, none.fb).length > 0, "an automated cell rendered like an idle one");
/* No hook, no change: an io without isAutomated is the old controller. */
const old = rig({ isModulated: () => false });
check(diff(old.fb, none.fb).length === 0, "an io without isAutomated must draw exactly as before");

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("PASS: automated mark (2x2 beside the label) and lane motion via io.isAutomated");
'
