#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE PICTURE IS ONE DOOR, AND THE FILE IS NOT PART OF IT.
#
# granny draws `position`, `spray` and `sample_path` on three adjacent knobs.
# Three reports from the device, in order, and each one falsified the fix for
# the one before:
#
#   "empty sample selection is indistinguishable from the spray control"
#       -- the filepath was SWALLOWED by the waveform, so it drew no frame, no
#          chevron and no filename. Bracketing it was the first answer.
#   "shouldn't the whole thing be divable?"
#       -- `spray` opened nothing, so one picture had a door on the left third
#          and nothing in the middle. vizDiveTarget was the second answer.
#   "sample file isn't part of the continuum because it goes to a different
#    editor" / "why is there a line that spans between them?"
#       -- and that is the real one. The file is not a position within the
#          sample, it is how you replace the sample, so it must not be inside
#          the sample's picture at all.
#
# The settled shape, which is what this file pins:
#
#   position + spray   one waveform, ONE bracket, opens the wave editor
#   sample_path        its own opaque box: notched frame, chevron, FILENAME
#
# Asserted on PIXELS and on the resolver, because none of it is visible in a
# code review of the draw call -- every version above reads plausibly.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { renderPageMovy, displayValue } from "./src/shared/param_pages/render_page_movy.mjs";
import { buildMetaIndex, KIND_OPAQUE } from "./src/shared/param_pages/param_meta.mjs";
import { resolveViz, VIZ_SAMPLE } from "./src/shared/param_pages/viz.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "  ok  " : "  FAIL ") + m); if (!c) fail++; };

/* granny in miniature. */
const CHAIN_PARAMS = [
  { key: "position", name: "Pos", type: "float", ui_type: "wav_position",
    min: 0, max: 1, step: 0.01 },
  { key: "spray", name: "Spray", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "sample_path", name: "Smp File", type: "filepath" },
  { key: "size", name: "Size", type: "float", min: 0, max: 1, step: 0.01 },
];
const KEYS = ["position", "spray", "sample_path", "size"];
const metaIndex = buildMetaIndex({ chainParams: CHAIN_PARAMS });
const page = { kind: "knobs", name: "Main", title: "Main", keys: KEYS };

const CELL_W = 32, ROW0_Y = 9, BOX_H = 15;

/* ---- 1. the resolver: the file keeps its own cell ---------------------- */

const { groups } = resolveViz({ keys: KEYS, metaIndex });
const sample = groups.filter((g) => g.kind === VIZ_SAMPLE);
ok(sample.length === 1, "one sample graphic is detected");
const g0 = sample[0];
ok(g0.slotStart === 0 && g0.slotSpan === 2,
   "the graphic spans position+spray ONLY (got start " + g0.slotStart +
   " span " + g0.slotSpan + ") -- claiming the file is what drew a line across " +
   "a boundary where the behaviour changes");
ok((g0.keys || []).indexOf("sample_path") < 0,
   "the file does not claim a cell");
/* ...but the picture is still drawn FROM it. Losing this makes the waveform
 * blank while everything above still passes. */
ok(g0.roles && g0.roles.value === "sample_path",
   "the file is still roles.value -- the waveform is drawn FROM it, not ON it");

/* ---- 2. the pixels ------------------------------------------------------ */

function render(values) {
  const lit = new Set();
  /* Colour 0 must ERASE: notchCorners knocks the four corners out with
   * fillRect(..., 0), and that notch is the only thing separating the opaque
   * frame from the brackets -- they occupy the identical rect. */
  const ctx = {
    fillRect: (x, y, w, h, c) => {
      for (let yy = y; yy < y + h; yy++)
        for (let xx = x; xx < x + w; xx++) {
          if (c === 0) lit.delete(xx + "," + yy); else lit.add(xx + "," + yy);
        }
    },
    print: () => {}, drawText: () => {}, clear: () => {},
    measureText: (s) => String(s).length * 4,
  };
  renderPageMovy(ctx, { page, values, metaIndex, held: [], viz: groups });
  return lit;
}

const BRACKET_LEN = 4;
function bracketPixels(col, spanCols) {
  const x = col * CELL_W + 1, y = ROW0_Y, w = spanCols * CELL_W - 2, h = BOX_H;
  const px = [];
  for (let i = 0; i < BRACKET_LEN; i++)
    px.push([x + i, y], [x + w - 1 - i, y], [x + i, y + h - 1], [x + w - 1 - i, y + h - 1]);
  for (let i = 0; i < BRACKET_LEN - 1; i++)
    px.push([x, y + i], [x + w - 1, y + i], [x, y + h - 1 - i], [x + w - 1, y + h - 1 - i]);
  return px;
}
const on = (lit, x, y) => lit.has(x + "," + y);
const topMiddle = (col, spanCols) =>
  [col * CELL_W + Math.floor(spanCols * CELL_W / 2), ROW0_Y];

/* Brackets: all arms lit AND the middle of the top edge dark (a continuous
 * frame has that middle; corner stubs do not). */
function hasBrackets(lit, col, spanCols) {
  const tm = topMiddle(col, spanCols);
  return bracketPixels(col, spanCols).every(([x, y]) => on(lit, x, y))
      && !on(lit, tm[0], tm[1]);
}
/* A frame: that middle lit AND all four corners knocked out. Brackets drawn
 * over a frame fill the notch back in, which is the doubled border. */
function hasNotchedFrame(lit, col) {
  const x = col * CELL_W + 1, y = ROW0_Y, w = CELL_W - 2, h = BOX_H;
  const tm = topMiddle(col, 1);
  return on(lit, tm[0], tm[1])
      && !on(lit, x, y) && !on(lit, x + w - 1, y)
      && !on(lit, x, y + h - 1) && !on(lit, x + w - 1, y + h - 1);
}

const loaded = render({ position: "0.42", spray: "0.15",
                        sample_path: "/x/kick.wav", size: "0.5" });

ok(hasBrackets(loaded, 0, 2),
   "ONE bracket across the whole graphic -- per-cell drew the picture as boxes butted together");
ok(!hasBrackets(loaded, 0, 1) && !hasBrackets(loaded, 1, 1),
   "and NOT a second one inside it, on either cell");
ok(hasNotchedFrame(loaded, 2),
   "the file draws its OWN opaque box: notched frame and chevron, not brackets and not waveform");

/* THE spanning line, measured at the BOUNDARY COLUMN.
 *
 * Three probes for this and the first two measured the wrong thing, which is
 * worth recording because both looked right:
 *
 *   "is there ink in the file cell at midY" -- yes, always: the chevron sits
 *   at the vertical centre and so do the filename glyphs. Both belong there.
 *
 *   "how far does the run starting in the waveform reach" -- it stops at the
 *   first spray fence or the cursor, around x=35, long before the boundary.
 *   That PASSED under a deliberate mutation that put the file back inside the
 *   graphic, so it was proving nothing at all.
 *
 * x = 2*CELL_W is the 1px gutter between the graphic (which ends at
 * 2*CELL_W - 1) and the opaque boxs frame (which starts at 2*CELL_W + 1).
 * With the file claimed, the waveform runs straight through it; released, it
 * is the gap. One pixel, and it is the entire reported symptom.
 */
const midY = ROW0_Y + Math.floor(BOX_H / 2);
ok(on(loaded, CELL_W, midY), "the waveform baseline does run through the spray cell");
ok(!on(loaded, 2 * CELL_W, midY),
   "and no line spans between them: the gutter column at x=" + (2 * CELL_W) + " is clear");

/* ---- 3. empty vs unanswered, on the file cell -------------------------- */

const opaque = { kind: KIND_OPAQUE };
ok(displayValue("", opaque) === "NONE",
   "an empty file reads NONE, on the file cell where the emptiness belongs");
ok(displayValue(null, opaque) === "--",
   "a FAILED read still reads -- : collapsing the two is the tri-state bug, and it " +
   "would report an empty slot for granny every time its blocking WAV load stalls a read");
ok(displayValue("/a/b/kick.wav", opaque) === "kick.wav",
   "a loaded file reads its basename");

/* The word has to FIT the box, which is why it is not EMPTY: the 4x5 face has
 * to clear the chevron, leaving CELL_W - 2 - 9 = 21px, and EMPTY measures 23. */
const { fontWidth4x5 } = await import("./src/shared/param_pages/font4x5.mjs");
ok(fontWidth4x5(displayValue("", opaque)) <= CELL_W - 2 - 9,
   "and it fits the box budget rather than truncating to a stump");

/* ---- 4. the whole picture is a door ------------------------------------ */

/*
 * `spray` opens nothing of its own -- it is a plain float -- so a click on it
 * used to do nothing while a click one cell left opened the wave editor. One
 * picture, two behaviours: "shouldnt the whole thing be divable?"
 *
 * Driven through the REAL controller rather than by calling vizDiveTarget,
 * because the redirect has to survive onClick and come back out as an intent
 * naming the ANCHOR -- everything downstream (the editor entry, the
 * return-to-caller bookkeeping) is the unchanged position path and must be
 * handed the position.
 */
const C = await import("./src/shared/param_pages/page_controller.mjs");
const HIER = JSON.stringify({ modes: null, levels: { root: {
  label: "G", knobs: KEYS,
  params: KEYS.map((k) => ({ key: k })) } } });
const CP = JSON.stringify(CHAIN_PARAMS);
const ctl = C.createController({
  getParam: (k) => {
    const b = String(k).replace(/^[^:]+:/, "");
    if (b === "ui_hierarchy") return HIER;
    if (b === "chain_params") return CP;
    if (b === "sample_path") return "/x/kick.wav";
    return "0.5";
  },
  setParam: () => {}, announce: () => {},
});
ctl.load({ slot: 0, component: "synth" });
for (let i = 0; i < 8; i++) ctl.tick();

const slotOf = (k) => (ctl.page.keys || []).indexOf(k);
const sPos = slotOf("position"), sSpray = slotOf("spray"),
      sFile = slotOf("sample_path"), sSize = slotOf("size");
ok(sPos >= 0 && sSpray >= 0 && sFile >= 0 && sSize >= 0,
   "the fixture put all four params on the grid");

const posIntent = ctl.onClick(sPos);
ok(!!posIntent && posIntent.key === "position",
   "clicking the cursor opens the wave editor, as it always did");

const sprayIntent = ctl.onClick(sSpray);
ok(!!sprayIntent && sprayIntent.key === "position",
   "clicking SPRAY opens the same editor -- the intent names the anchor, not the cell");

const fileIntent = ctl.onClick(sFile);
ok(!!fileIntent && fileIntent.key === "sample_path",
   "clicking the FILE opens the file browser and is NOT redirected -- it is a different door");

/* The redirect must not leak to a cell that merely sits near a graphic. */
ok(ctl.onClick(sSize) === null,
   "an ordinary float outside the picture still opens nothing");

if (fail) { console.log("FAIL: " + fail + " assertion(s)"); process.exit(1); }
console.log("PASS: the sample graphic is one door, and the file is its own");
'
