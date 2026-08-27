#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Every enum in the knob grid is a DOOR as well as a knob.
#
# Clicking a held enum knob opens a scrolling list of that param's options; the
# knob itself keeps stepping the value exactly as it did. Two abilities, one
# param, and the codebase already knows they are separate questions -- see the
# long comment at param_meta.mjs `normalize`, where conflating `divable` with
# `opaque` made granny's Position knob dead.
#
# THE MARK IS THE PART THAT MUST NOT MOVE.
#
# render_page_movy draws corner brackets on a cell to say "you can go into
# this". There are 135 enum params in the fleet against 5 filepath/string/canvas
# ones, so bracketing every enum would put the mark on almost every cell on
# almost every page and it would stop meaning anything.
#
# So `divable` (does clicking open something) and the MARK are decoupled, and
# this pins ALL THREE CASES on the actual pixel buffer:
#
#   - an enum cell draws NO brackets
#   - a wav_position cell still DOES -- it is an opaque TYPE without being
#     KIND_OPAQUE, draws as a KNOB, and so has no frame but the brackets
#   - a filepath / canvas / string cell is a DOOR: since SCH-50 `door-open` it
#     draws its own notched frame with the right edge broken and a chevron in
#     the gap, and therefore no brackets. It used to have no frame at all,
#     which is why the brackets were structural for it; an option that removes
#     them has to replace them, and this one does. The rule being pinned is
#     "an opaque cell is visibly a door", not "an opaque cell is bracketed".
#
# All three are checked POSITIVELY. An assertion that only looked for absent
# brackets would pass on a widget that drew nothing at all.
#
# A test that only checked "no mark on an enum" would pass trivially if the
# mark were broken everywhere, which is the failure mode this file exists for.
#
# Part 3 checks that what the picker COMMITS is a value the plugin accepts,
# against chord -- whose set_param is a strcmp ladder over the option names
# with no trailing else, so an index on the wire is silently discarded. The
# accepted set is parsed out of chord.c, the same way
# test_enum_wire_format_contract.sh does it, so the test and the DSP cannot
# drift.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the enum picker tests" >&2
  exit 1
fi

node --input-type=module -e '
const [PM, PF, RM, H, fs] = await Promise.all([
  import("./src/shared/param_pages/param_meta.mjs"),
  import("./src/shared/param_format.mjs"),
  import("./src/shared/param_pages/render_page_movy.mjs"),
  import("./tools/param-pages/harness.mjs"),
  import("node:fs"),
]);

let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };

/* ======================================================================= 1 ==
 * An enum is DIVABLE and still TURNABLE.
 */
const CHAIN_PARAMS = [
  { key: "mode",   name: "Mode",  type: "enum", options: ["Hall", "Room", "Plate", "Cave"] },
  { key: "bare",   name: "Bare",  type: "enum" },                       /* no options */
  { key: "sw",     name: "Sw",    type: "toggle" },                     /* two-option enum */
  { key: "file",   name: "File",  type: "filepath" },
  { key: "view",   name: "View",  type: "canvas" },
  { key: "name",   name: "Name",  type: "string" },
  { key: "pos",    name: "Pos",   type: "wav_position", min: 0, max: 1, step: 0.01 },
  { key: "gain",   name: "Gain",  type: "float", min: 0, max: 1, step: 0.01 },
];
const mi = PM.buildMetaIndex({ hierarchy: null, chainParams: CHAIN_PARAMS });
const m = (k) => mi.getOrGuess(k);

if (!PM.isDivable(m("mode")))
  fail("an enum with declared options must be divable -- clicking a held enum " +
       "knob is how you get its option list");
if (m("mode").kind !== PM.KIND_ENUM)
  fail("an enum must stay KIND_ENUM so the knob keeps stepping it; got " + m("mode").kind);
if (!PM.isTurnable(m("mode")))
  fail("making an enum divable must not make it opaque -- divable and turnable " +
       "are independent, and conflating them is the bug param_meta.mjs already " +
       "documents for wav_position");
if (!PM.isDivable(m("sw")))
  fail("a `toggle` normalises to a two-option enum, so it is divable too");
if (PM.isDivable(m("bare")))
  fail("an enum that declares NO options has no list to show -- it must not " +
       "advertise a door that opens on nothing");
if (PM.isDivable(m("gain")))
  fail("a plain float is not divable");
for (const k of ["file", "view", "name", "pos"])
  if (!PM.isDivable(m(k))) fail(k + " must still be divable");

/* The knob still moves it. Drive the real turn through the controller. */
{
  const { createController } = await import("./src/shared/param_pages/page_controller.mjs");
  const hierarchy = { modes: null, levels: { root: { label: "T",
    knobs: ["mode", "gain", "file", "pos"],
    params: CHAIN_PARAMS.map((p) => ({ key: p.key })) } } };
  const store = { mode: "0", gain: "0.5", file: "", pos: "0.25" };
  const ctl = createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(hierarchy);
      if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
      return b in store ? store[b] : "";
    },
    setParam: (k, v) => { store[String(k).replace(/^[^:]+:/, "")] = String(v); },
    announce: () => {},
  });
  ctl.load({ prefix: "synth" });
  const slot = (ctl.page.keys || []).indexOf("mode");
  if (slot < 0) fail("mode never reached the page");
  else {
    const before = store.mode;
    const t0 = Date.now();
    for (let i = 0; i < 24; i++) ctl.onKnobTurn(slot, 1, false, t0 + i * 60);
    ctl.tick();
    if (store.mode === before)
      fail("turning a divable enum knob wrote nothing -- it must still step the " +
           "value; got " + JSON.stringify(store.mode));
  }

  /* ...and clicking it while held asks the HOST to open something. */
  if (slot >= 0) {
    ctl.onKnobTouch(slot, true);
    const todo = ctl.onClick(slot);
    if (!todo || todo.action !== "open")
      fail("clicking a held enum knob returned " + JSON.stringify(todo) +
           ", expected an {action:\"open\"} intent for the host");
    if (todo && todo.key !== "mode") fail("the open intent names the wrong key: " + todo.key);
    ctl.onKnobTouch(slot, false);
  }
}

/* ======================================================================= 2 ==
 * THE MARK, on the pixels, in both directions.
 */
{
  /* One row of four cells: enum, filepath, canvas, wav_position. */
  const page = { kind: "knobs", keys: ["mode", "file", "view", "pos",
                                       null, null, null, null] };
  const values = { mode: "1", file: "/x/kick.wav", view: "", pos: "0.25" };
  const fb = H.createFramebuffer();
  RM.drawKnobRow(H.drawContext(fb), { page, metaIndex: mi, values, touched: -1 },
                 0, RM.ROW0_Y, RM.LBL0_Y);
  if (fb.clipped() !== 0) fail("the row drew outside the display");

  const at = (x, y) => fb.pixels[y * fb.width + x];
  /* drawDivableMark: cellLeft + 1, rowY, cellW - 2, BOX_H */
  function corners(col) {
    const x0 = col * RM.CELL_W + 1, y0 = RM.ROW0_Y;
    const w = RM.CELL_W - 2, h = RM.BOX_H;
    return [[x0, y0, "top left"], [x0 + w - 1, y0, "top right"],
            [x0, y0 + h - 1, "bottom left"], [x0 + w - 1, y0 + h - 1, "bottom right"]];
  }
  function marked(col) {
    return corners(col).every(([x, y]) => at(x, y));
  }
  function unmarked(col) {
    return corners(col).every(([x, y]) => !at(x, y));
  }

  /* The enum: NO mark. */
  if (!unmarked(0)) {
    const lit = corners(0).filter(([x, y]) => at(x, y)).map((c) => c[2]);
    fail("the ENUM cell drew divable brackets (" + lit.join(", ") + "). 135 of the " +
         "fleet`s params are enums against 5 opaque ones, so bracketing them all " +
         "makes the mark mean nothing. The affordance is the footer`s CLK OPEN.");
  }
  /*
   * wav_position STILL wears the brackets, and it is the whole reason the two
   * fields are separate. It is an opaque TYPE without being KIND_OPAQUE: a
   * ranged number a knob turns perfectly well that ALSO has a waveform editor
   * worth opening. It draws as a knob, so it has no frame of its own and the
   * brackets remain its only mark.
   */
  if (!marked(3)) {
    const dark = corners(3).filter(([x, y]) => !at(x, y)).map((c) => c[2]);
    fail("the wav_position cell has lost its divable brackets (" + dark.join(", ") +
         "). It draws as a KNOB -- an opaque type but not KIND_OPAQUE -- so " +
         "the brackets are the only thing saying it opens an editor.");
  }

  /*
   * THE OPAQUE TYPES NO LONGER WEAR BRACKETS, and that is a swap rather than a
   * loss.
   *
   * The premise this block used to assert -- "drawOpaqueBox draws no frame of
   * its own, so the brackets ARE its frame" -- stopped being true with SCH-50
   * `door-open`. The cell now draws a notched frame with its RIGHT EDGE BROKEN
   * and a chevron in the gap, which is a stronger statement of the same
   * affordance: it is the only cell on the page that says which DIRECTION its
   * door goes. Brackets on top of that frame would sit on the same rect and
   * read as a doubled border.
   *
   * So the rule that must not move is not "opaque cells are bracketed", it is
   * "an opaque cell is visibly a door". Asserted on the pixels as: corners
   * notched, a frame present, the right edge OPEN over the chevron rows, and a
   * chevron in the opening. Checking only the absence of brackets would pass if
   * the widget drew nothing at all, which is the failure mode this whole file
   * exists for.
   */
  function doorOk(atFn, col, what) {
    const x = col * RM.CELL_W + 1, y = RM.ROW0_Y;
    const w = RM.CELL_W - 2, h = RM.BOX_H;
    const gapY = y + ((h - 5) >> 1);
    for (const [cx, cy, which] of corners(col))
      if (atFn(cx, cy)) fail("the " + what + " door frame is not notched at its " + which + " corner");
    if (!atFn(x + Math.floor(w / 2), y))
      fail("the " + what + " cell drew no top frame edge -- it is not a door, it is nothing");
    if (!atFn(x, y + Math.floor(h / 2)))
      fail("the " + what + " cell drew no left frame edge");
    for (let i = 0; i < 5; i++)
      if (atFn(x + w - 1, gapY + i))
        fail("the " + what + " door has no opening -- its right edge is closed at row " + (gapY + i));
    if (!atFn(x + w - 1, gapY - 1) || !atFn(x + w - 1, gapY + 5))
      fail("the " + what + " door opening is not bounded by the right edge above and below it");
    let chevron = 0;
    for (let dx = 0; dx < 3; dx++)
      for (let i = 0; i < 5; i++) if (atFn(x + w - 4 + dx, gapY + i)) chevron++;
    if (chevron < 5)
      fail("the " + what + " door opening has only " + chevron + " chevron pixel(s) in it -- " +
           "the mark that says which way the door goes is missing");
  }
  for (const [col, what] of [[1, "filepath"], [2, "canvas"]]) doorOk(at, col, what);

  /* And a `string`, which is opaque too but is what an LFO Target declares. */
  {
    const fb2 = H.createFramebuffer();
    RM.drawKnobRow(H.drawContext(fb2),
      { page: { kind: "knobs", keys: ["name", null, null, null, null, null, null, null] },
        metaIndex: mi, values: { name: "fx1" }, touched: -1 },
      0, RM.ROW0_Y, RM.LBL0_Y);
    doorOk((x, y) => fb2.pixels[y * fb2.width + x], 0, "string");
  }
  if (failures === 0) console.log("PASS: enums are unmarked, wav_position keeps its brackets, opaque cells are doors");
}

/* ======================================================================= 3 ==
 * What the picker COMMITS has to be a value the plugin accepts.
 */
{
  const mj = JSON.parse(fs.readFileSync("src/modules/midi_fx/chord/module.json", "utf8"));
  const declared = mj.capabilities.ui_hierarchy.levels.root.params.find((p) => p.key === "type");
  const csrc = fs.readFileSync("src/modules/midi_fx/chord/dsp/chord.c", "utf8");
  const at = csrc.indexOf("(key, \"type\")");
  const next = csrc.indexOf("strcmp(key,", at + 1);
  const branch = csrc.slice(at, next < 0 ? csrc.length : next);
  const accepted = new Set();
  for (const mm of branch.matchAll(/strcmp\(val,\s*"([^"]+)"\)/g)) accepted.add(mm[1]);
  if (accepted.size === 0) fail("parsed no accepted names out of chord.c");

  /* Through the same pipeline the grid uses: hierarchy + the chain_params the
     chain host renders (which does NOT emit options_as_string). */
  const idx = PM.buildMetaIndex({
    hierarchy: mj.capabilities.ui_hierarchy,
    chainParams: [{ key: "type", name: "Type", type: "enum", options: declared.options }],
  });
  const meta = idx.getOrGuess("type");

  if (typeof PF.enumWireValue !== "function")
    fail("param_format.mjs exports no enumWireValue -- the picker and the knob " +
         "must decide the wire format the same way, in one place, or the picker " +
         "will write String(index) into a plugin that only accepts names");
  else {
    /* Every option, not one: a single pick passes on `major` by luck. */
    const bad = [];
    for (let i = 0; i < declared.options.length; i++) {
      /* `currentRaw` is what the plugin REPORTED, which is how the format is
         detected -- chord reports names. */
      const wire = PF.enumWireValue(meta, i, "major");
      if (!accepted.has(wire)) bad.push([i, wire]);
    }
    if (bad.length)
      fail("chord would silently discard " + bad.length + " of " +
           declared.options.length + " picks -- e.g. " + JSON.stringify(bad.slice(0, 3)) +
           ". chord_set_param is a strcmp ladder over the NAMES with no trailing " +
           "else, so those leave `type` where it was while the UI shows it moved.");

    /* ...and an index-speaking plugin still gets an index. */
    const im = PM.buildMetaIndex({ hierarchy: null, chainParams: [
      { key: "shape", name: "Shape", type: "enum", options: ["Sine", "Saw", "Sqr"] },
    ] }).getOrGuess("shape");
    const w = PF.enumWireValue(im, 2, "0");   /* plugin reported the index "0" */
    if (w !== "2")
      fail("a plugin that reports indices must receive an index, got " + JSON.stringify(w));
  }
  if (failures === 0) console.log("PASS: the picker commits values chord actually accepts");
}

if (failures) process.exit(1);
console.log("PASS: enums are divable and turnable, unmarked, and commit a wire " +
            "value the plugin accepts");
'
