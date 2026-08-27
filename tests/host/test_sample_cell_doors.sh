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
/*
 * The probe point that separates a CONTINUOUS top edge from corner stubs: the
 * first column past the left bracket arm.
 *
 * The obvious point is the middle of the top edge, and it is wrong -- the arc
 * knobs curve reaches the top of its cell too, so a bracketed KNOB failed the
 * test for having a widget in it. One column past the arm is outside the knob
 * entirely (the widget is 17px centred in a 32px cell, so it starts at +7)
 * while still being on any frame that spans the cell.
 */
const topPastArm = (col) => [col * CELL_W + 1 + BRACKET_LEN, ROW0_Y];

/* Brackets: all arms lit AND no continuous edge between them. */
function hasBrackets(lit, col, spanCols) {
  const t = topPastArm(col);
  return bracketPixels(col, spanCols).every(([x, y]) => on(lit, x, y))
      && !on(lit, t[0], t[1]);
}
/* A frame: that edge lit AND all four corners knocked out. Brackets drawn over
 * a frame fill the notch back in, which is the doubled border. */
function hasNotchedFrame(lit, col) {
  const x = col * CELL_W + 1, y = ROW0_Y, w = CELL_W - 2, h = BOX_H;
  const t = topPastArm(col);
  return on(lit, t[0], t[1])
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

/* ---- 3b. no file: MARKERS still draw, a file-only graphic does not ------
 *
 * "You should see the loaded break, but not an empty waveform" was reported
 * against breakbeat, whose A SMP / B SMP cells are graphics built from a
 * filepath ALONE -- nothing loaded means nothing to draw, so they were a
 * bracketed rectangle containing precisely nothing. Those collapse to the
 * ordinary opaque box.
 *
 * Suppressing EVERY empty sample graphic was too broad, and this fixture is
 * the case that shows why: position + spray are two real controls whose
 * picture is the track they act on, and empty they still want it -- it is
 * where the cursor and the fences live. "When no sample is loaded it should
 * be the empty two column widget."
 *
 * So the test is MARKERS, not emptiness.
 */
{
  const empty  = render({ position: "0.42", spray: "0.15", sample_path: "", size: "0.5" });
  const unread = render({ position: "0.42", spray: "0.15", size: "0.5" });
  const midY2 = ROW0_Y + Math.floor(BOX_H / 2);

  ok(on(empty, CELL_W, midY2),
     "with no file the MARKER graphic still draws -- position and spray are real " +
     "controls and the track is their picture");
  ok(hasBrackets(empty, 0, 2),
     "and it keeps its two-cell span, not two separate knobs");
  ok(hasNotchedFrame(empty, 2),
     "while the FILE cell falls back to its own box, which is where NONE belongs");
  ok(on(unread, CELL_W, midY2),
     "an UNANSWERED read draws it too -- a failed read empties nothing");
}

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

/* ---- 5. an EXTRA key EMPTY answer must be stored ---------------------
 *
 * granny declares sample_path on no knobs list, so on its root page the file
 * is OFF-PAGE and reaches the controller as a viz EXTRA key -- a separate stop
 * in the read rotation from the ordinary per-key one.
 *
 * That stop used to drop `""` along with null, on the reasoning that a failed
 * read must not become "no sample". True of null; false of `""`, which is the
 * channel saying there IS no file. Dropped, the value stayed UNDEFINED
 * forever, and undefined is indistinguishable from null downstream: the cell
 * reads "--" instead of NONE, and the empty-graphic suppression (keyed on
 * `=== ""`) never fires, so an empty sample still drew the empty waveform.
 *
 * Reported as "why does granny show -- instead of none". The device log
 * settled it: across 41MB, `sample_path` never once appears in a
 * param_giveup, so the read had been succeeding all along.
 *
 * Driven through the real controller, with the file deliberately OFF the page.
 */
{
  /* sample_path appears in NEITHER knobs nor params, which is what makes it
   * off-page. It is still found, because `position` names it through
   * filepath_param -- that declaration is how detectSample resolves a file it
   * cannot see on the page, and it is what granny does. Listing it in params
   * put it back ON the page and the store assertion below then passed through
   * the ordinary per-key read, proving nothing. */
  const CP2 = CHAIN_PARAMS.map((m) => (m.key === "position"
      ? Object.assign({}, m, { filepath_param: "sample_path" }) : m));
  const HIER2 = JSON.stringify({ modes: null, levels: { root: {
    label: "G", knobs: ["position", "spray", "size"],
    params: [{ key: "position" }, { key: "spray" }, { key: "size" }] } } });
  const reads = [];
  const mk = (pathValue) => {
    const c2 = C.createController({
      getParam: (k) => {
        const b = String(k).replace(/^[^:]+:/, "");
        reads.push(b);
        if (b === "ui_hierarchy") return HIER2;
        if (b === "chain_params") return JSON.stringify(CP2);
        if (b === "sample_path") return pathValue;
        if (b === "preset_name") return null;
        return "0.5";
      },
      setParam: () => {}, announce: () => {},
    });
    c2.load({ slot: 0, component: "synth" });
    /* Long enough for the rotation to reach the extra stop several times. */
    for (let i = 0; i < 60; i++) c2.tick();
    return c2;
  };

  const empty = mk("");
  ok((empty.page.keys || []).indexOf("sample_path") < 0,
     "the fixture keeps the file OFF the page, so it is an extra key");
  ok(reads.indexOf("sample_path") >= 0,
     "the extra-key stop actually read it");
  ok(empty.state.values.sample_path === "",
     "an EMPTY answer is stored as the empty string -- dropping it leaves " +
     "undefined, which renders -- and defeats the empty-graphic suppression " +
     "(got " + JSON.stringify(empty.state.values.sample_path) + ")");

  /* ...and a MISSING answer is still not stored, which is the half that was
   * right. Distinguishing them is the whole point. */
  const failed = mk(null);
  ok(failed.state.values.sample_path === undefined,
     "a FAILED read is still not stored (got " +
     JSON.stringify(failed.state.values.sample_path) + ")");
}

/* ---- 6. an ON-PAGE empty filepath is stored too ------------------------
 *
 * The other half of the same collapse, and the one that actually bit.
 *
 * The ordinary per-key read treats "" as a MISS -- right for a number (an
 * unserved key answering "" once showed Volume as Number("") = 0) and wrong
 * for a filepath, where "" is the module saying there is no file.
 *
 * Reported as a sequence, which is what named it: "it showed none, i clicked
 * into it, went back and it showed --", permanently. On granny the file is
 * off-page on the ROOT page, so the viz extra-key stop stored "" and put NONE
 * on screen; values accumulate across pages, so that survived a jog to the
 * page where the file IS a key. Returning from the browser rebuilds the
 * controller, emptying values -- and on that page the read comes through the
 * ordinary path, which threw the answer away every rotation.
 *
 * Invisible in the device log, because the read SUCCEEDS: across 41MB
 * sample_path never once appears in a param_giveup.
 *
 * Driven with the file ON the page, which is the case the extra-key test
 * cannot cover.
 */
{
  const HIER3 = JSON.stringify({ modes: null, levels: { root: {
    label: "G", knobs: ["position", "spray", "size"],
    params: [{ key: "position" }, { key: "spray" }, { key: "size" },
             { key: "sample_path" }] } } });
  const mk3 = (pathValue, sizeValue) => {
    const c3 = C.createController({
      getParam: (k) => {
        const b = String(k).replace(/^[^:]+:/, "");
        if (b === "ui_hierarchy") return HIER3;
        if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
        if (b === "sample_path") return pathValue;
        if (b === "size") return sizeValue;
        if (b === "preset_name") return null;
        return "0.5";
      },
      setParam: () => {}, announce: () => {},
    });
    c3.load({ slot: 0, component: "synth" });
    for (let i = 0; i < 60; i++) c3.tick();
    return c3;
  };

  const onPage = mk3("", "0.5");
  ok((onPage.page.keys || []).indexOf("sample_path") >= 0,
     "this fixture puts the file ON the page, so it uses the ordinary read");
  ok(onPage.state.values.sample_path === "",
     "an empty filepath read from the PAGE is stored -- discarding it leaves " +
     "undefined, which is the permanent -- (got " +
     JSON.stringify(onPage.state.values.sample_path) + ")");

  /* The number rule must NOT have moved. An unserved numeric key answering ""
   * is still a miss; storing it would show Number("") = 0, which is the
   * slot-settings Volume bug. */
  const numEmpty = mk3("/x/kick.wav", "");
  ok(numEmpty.state.values.size === undefined,
     "an empty NUMBER is still a miss (got " +
     JSON.stringify(numEmpty.state.values.size) + ")");
  ok(numEmpty.state.values.sample_path === "/x/kick.wav",
     "and a real filepath still reads normally");
}

if (fail) { console.log("FAIL: " + fail + " assertion(s)"); process.exit(1); }
console.log("PASS: the sample graphic is one door, and the file is its own");
'
