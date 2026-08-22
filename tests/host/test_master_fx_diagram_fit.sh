#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The Master FX row must FIT, at whatever the cap is.
#
# What this replaces could not be asserted at all. drawMasterFx drew a fixed
# row -- `TOTAL_W = 5 * BOX_W + 4 * GAP`, five 22px boxes filling 118 of the
# 128px screen exactly -- plus two more hand-rolled loops that walked the FULL
# component list to place the bypass "B" and the LFO "~" markers. A fixed row
# has no way to report that it overflowed: at the 8-slot cap the nine boxes are
# 214px wide and boxes 6..9 and their markers were simply drawn off the right
# edge, silently, with the device discarding the pixels. Nine source-text greps
# covered Master FX and every one of them passed in that state.
#
# So this renders the real function into the real framebuffer and counts the
# pixels that landed outside the display. Zero is the assertion; anything else
# is a box, a label or a marker the user will never see.
#
# It also pins the READ BUDGET, because the fix and the regression look the
# same from the outside. drawMasterFx runs every frame while the screen is up
# and an IPC round trip is ~2.8ms against a 1.68ms whole-page render, so a row
# that fetches bypass state for all N slots costs more at 8 than at 4 even
# though it now looks right. The windowed diagram fetches per DRAWN box, so the
# cost is bounded by the diagram capacity and does not grow with the cap.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node --input-type=module -e '
/* The REAL shiftHintsFor from the shared chrome. These renders are pixel
   baselines, so a stub would bake in a footer nobody actually draws. */
const CHROME_SHIFT_HINTS = (await import("./src/shared/chain_editor_chrome.mjs")).shiftHintsFor;
import { readFileSync } from "node:fs";
import { createFramebuffer } from "./tools/param-pages/harness.mjs";
import { drawChainDiagram, DIAGRAM_W, BOX_H as DIAGRAM_BOX_H, CAPACITY,
         DEFAULT_Y as DIAGRAM_Y }
  from "./src/shared/chain_diagram.mjs";
/* The shared bands, REAL: they draw the header band and the hint footer, and
   the off-display check below is about what actually reaches the screen. */
import { drawChainEditorBands } from "./src/shared/chain_editor_chrome.mjs";
/* The knob card, REAL. It is a module import in shadow_ui_master_fx.mjs, so
   under the lift it is a free identifier and must be a dependency -- and it
   draws OVER the diagram, which is exactly the kind of thing that can put
   pixels off the display. Case 6 renders one. */
import { drawKnobCard } from "./src/shared/param_pages/knob_card.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { parseId as parseChainId, chainComponents } from "./src/shared/chain_model.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const jsSrc = readFileSync("src/shadow/shadow_ui.js", "utf8");
const mjsSrc = readFileSync("src/shadow/shadow_ui_master_fx.mjs", "utf8");

/* The cap is READ from source, never restated here: a test that hardcodes 8
   stops testing the thing it exists for the moment somebody raises it. */
const capM = jsSrc.match(/^const MASTER_FX_SLOTS = (\d+);/m);
if (!capM) { console.error("FAIL: could not read MASTER_FX_SLOTS"); process.exit(1); }
const CAP = parseInt(capM[1], 10);

/* The Master FX declaration block, evaluated out of shadow_ui.js rather than
   reconstructed -- the list the device draws is the list under test. The list
   is now DERIVED from the loaded chain through chainEditorComponents, so that
   is lifted and handed in too. */
function componentsAtCap(cap) {
  let block = jsSrc.slice(jsSrc.indexOf("const MASTER_FX_SLOTS = "),
                          jsSrc.indexOf("let selectingMasterFxModule"));
  block = block.replace(/const MASTER_FX_SLOTS = \d+;/, "const MASTER_FX_SLOTS = " + cap + ";");
  const ceAt = jsSrc.indexOf("function chainEditorComponents(");
  const chainEditorComponents = new Function("chainComponents",
    jsSrc.slice(ceAt, jsSrc.indexOf("\n}\n", ceAt) + 2) +
    "\nreturn chainEditorComponents;")(chainComponents);
  return new Function("parseChainId", "chainEditorComponents", block +
    "return {masterFxChainComponents, makeEmptyMasterFxConfig, MASTER_CHAIN_TARGET," +
    " setConfig: (c) => { masterFxConfig = c; }};")(
    parseChainId, chainEditorComponents);
}

/* drawMasterFx paints its LFO and bypass markers through the SHARED helpers
   the slot chain editor uses, reached over ctx. They are lifted out of
   shadow_ui.js rather than stubbed, because what this file measures is the
   READ COUNT and a stub would be measuring the stub. */
function liftJs(name, deps) {
  const at = jsSrc.indexOf("function " + name + "(");
  const end = at >= 0 ? jsSrc.indexOf("\n}\n", at) : -1;
  if (end < 0) { console.error("FAIL: " + name + " is gone from shadow_ui.js"); process.exit(1); }
  return new Function(...deps, jsSrc.slice(at, end + 2) + "\nreturn " + name + ";");
}

/* drawMasterFx cannot be imported -- the module imports by absolute device
   path -- but it can be LIFTED and handed its dependencies as parameters,
   which is the difference between pinning source text and pinning behaviour. */
const at = mjsSrc.indexOf("export function drawMasterFx()");
const end = at >= 0 ? mjsSrc.indexOf("\n}\n", at) : -1;
if (end < 0) { console.error("FAIL: drawMasterFx is gone"); process.exit(1); }
const body = mjsSrc.slice(at, end + 2).replace("export function", "function");

function render(cap, selected, opts = {}) {
  const decls = componentsAtCap(cap);
  const fb = createFramebuffer();
  const reads = [];
  const bypassed = opts.bypassed || new Set();
  const lfo = opts.lfo || {};
  const shadow_get_param = (slot, key) => {
    reads.push(key);
    if (key.endsWith(":bypassed")) return bypassed.has(key.split(":")[1]) ? "1" : "0";
    const l = key.match(/lfo(\d):(enabled|target)$/);
    if (l) return l[2] === "enabled" ? (lfo[l[1]] ? "1" : "0") : (lfo[l[1]] || "");
    return "";
  };
  const cfg = decls.makeEmptyMasterFxConfig();
  /* Every slot loaded: the worst case for both width and read count, and the
     only state in which the component list reaches the cap at all -- it is the
     CHAIN that bounds it now, not the array size. */
  if (!opts.empty) for (let i = 1; i <= cap; i++) cfg["fx" + i].module = "module" + i;
  decls.setConfig(cfg);

  const uictx = {
    masterShowingNamePreview: false, masterConfirmingOverwrite: false,
    masterConfirmingDelete: false, helpDetailScrollState: null, helpNavStack: [],
    inMasterPresetPicker: false, inMasterFxSettingsMenu: false,
    selectingMasterFxModule: false,
    selectedMasterFxComponent: selected,
    masterFxConfig: cfg,
    /* A GETTER, as the device ctx has: the list changes length on every shape
       edit, and a snapshot taken here would be the fixed row this step removed. */
    get MASTER_FX_CHAIN_COMPONENTS() { return decls.masterFxChainComponents(); },
    ensureMasterFxConfigFresh: () => {},
    isShiftHeld: () => !!opts.shift,
    MASTER_FX_OPTIONS: [],
    currentMasterPresetName: "",
    getMasterFxParam: (i, k) => { reads.push("master_fx:fx" + (i + 1) + ":" + k); return ""; },
    /* Three characters, the longest a module.json abbrev is seen to be. */
    getModuleAbbrev: (id) => (id ? String(id).slice(0, 3).toUpperCase() : "--"),
    isTextEntryActive: () => false,
    drawTextEntry: () => {}, drawHelpDetail: () => {}, drawHelpList: () => {},
    /* Costs no IPC by construction: the card is handed its values, it never
       reads. A card here must not move the read budget below. */
    knobCardDrawState: () => (opts.card || null),
  };

  const getSlotParam = (slot, key) => shadow_get_param(slot, key);
  const chainTargetGetParam = liftJs("chainTargetGetParam", ["getSlotParam"])(getSlotParam);
  uictx.MASTER_CHAIN_TARGET = decls.MASTER_CHAIN_TARGET;
  uictx.chainLfoTargetMap = liftJs("chainLfoTargetMap", ["getSlotParam"])(getSlotParam);
  uictx.chainComponentBypassed =
    liftJs("chainComponentBypassed", ["chainTargetGetParam"])(chainTargetGetParam);

  const deps = {
    ctx: uictx,
    clear_screen: fb.clearScreen,
    fill_rect: fb.fillRect,
    draw_rect: (x, y, w, h, c) => {
      fb.fillRect(x, y, w, 1, c); fb.fillRect(x, y + h - 1, w, 1, c);
      fb.fillRect(x, y, 1, h, c); fb.fillRect(x + w - 1, y, 1, h, c);
    },
    print: fb.print,
    set_pixel: fb.setPixel,
    text_width: fb.textWidth,
    shadow_get_param,
    drawHeader: (t) => { fb.print(2, 0, String(t), 1); },
    drawChainDiagram, DIAGRAM_W, DIAGRAM_BOX_H, DIAGRAM_Y, drawChainEditorBands,
    /* REAL, like drawChainEditorBands beside it: the Shift footer is drawn
       from the shared chrome, and a stub would bake in a footer nobody draws. */
    shiftHintsFor: CHROME_SHIFT_HINTS,
    SCREEN_WIDTH: 128,
    truncateText: (t, n) => String(t == null ? "" : t).slice(0, n),
    drawMasterNamePreview: () => {}, drawMasterConfirmOverwrite: () => {},
    drawMasterConfirmDelete: () => {}, drawMasterPresetPicker: () => {},
    drawMasterFxSettingsMenu: () => {}, drawMasterFxModuleSelect: () => {},
    drawKnobCard,
  };
  const names = Object.keys(deps);
  new Function(...names, body + "\nreturn drawMasterFx;")(...names.map(n => deps[n]))();
  return { fb, reads };
}

/* ---- 1. nothing is drawn off the display, anywhere in the row ---------- *
 *
 * Both ends and the middle, plus the settings box and the preset row (-1,
 * which lights every box at once). The ends are where a windowed row fails:
 * the first and last windows are the two the scroll arithmetic clamps.
 */
/* CAP is the `+` box and CAP+1 is Settings once every position is loaded --
   both are drawn boxes and both are ends of the window. */
const SELECTIONS = [-1, 0, 1, Math.floor(CAP / 2), CAP - 1, CAP, CAP + 1];
for (const sel of SELECTIONS) {
  const { fb } = render(CAP, sel, {
    /* Markers on the first and last FX slot, so a marker drawn against an
       unwindowed x is caught too -- that is a separate overflow from the box
       itself, and the two old marker loops each had it. */
    bypassed: new Set(["fx1", "fx" + CAP]),
    lfo: { 1: "fx1", 2: "fx" + CAP },
  });
  if (fb.clipped() !== 0) {
    fail("cap " + CAP + ", selection " + sel + ": " + fb.clipped() +
         " pixels drawn OUTSIDE the 128x64 display - the row does not fit");
  }
  if (fb.missingGlyphs.size) {
    fail("cap " + CAP + ", selection " + sel + ": glyphs the device font lacks: " +
         [...fb.missingGlyphs].join(""));
  }
}

/* ---- 2. it still fits one cap higher ---------------------------------- *
 *
 * The point of adopting the shared diagram is that the layout stops having an
 * opinion about the count. If a future raise reintroduces a fixed row, this
 * catches it before the cap moves rather than after.
 */
{
  const { fb } = render(CAP + 1, CAP, {});
  if (fb.clipped() !== 0) {
    fail("at cap " + (CAP + 1) + " the row clips " + fb.clipped() +
         " pixels - the layout is bounded by something other than the window");
  }
}

/* ---- 3. the read budget does not grow with the cap --------------------- *
 *
 * A row that fetches bypass state for every slot costs 2 extra IPC round trips
 * per frame at 8 slots. Bypass reads must be bounded by how many boxes are
 * DRAWN, which is the diagram capacity, not by how many exist.
 */
{
  const big = render(CAP, 0, {});
  const small = render(4, 0, {});
  const bypassReads = (r) => r.filter((k) => k.endsWith(":bypassed")).length;
  if (bypassReads(big.reads) > CAPACITY) {
    fail("cap " + CAP + " selection 0 issues " + bypassReads(big.reads) +
         " bypass reads, more than the " + CAPACITY + " boxes that fit - the " +
         "row is reading per SLOT rather than per DRAWN box");
  }
  if (big.reads.length > small.reads.length + 2) {
    fail("cap " + CAP + " costs " + big.reads.length + " reads per frame against " +
         small.reads.length + " at 4 - the cap raise made the screen more " +
         "expensive, at ~2.8ms a read on a 60Hz redraw");
  }
  /* The LFO question is asked of the two LFOs, never of each box. */
  const lfoReads = big.reads.filter((k) => /lfo\d:/.test(k)).length;
  if (lfoReads > 4) {
    fail("cap " + CAP + " issues " + lfoReads + " LFO reads - it is asking " +
         "each box which LFO points at it, instead of asking the two LFOs");
  }
}

/* ---- 4. the settings box and the `+` are never treated as FX slots ----- */
{
  const { reads } = render(CAP, CAP + 1, {});
  for (const bad of ["master_fx:settings", "master_fx:add_fx"]) {
    if (reads.some((k) => k.indexOf(bad) === 0)) {
      fail("a non-module box was asked for a slot parameter: " +
           reads.filter((k) => k.indexOf(bad) === 0).join(" "));
    }
  }
}

/* ---- 4b. AN EMPTY CHAIN: one `+`, and it still fits ------------------- *
 *
 * The state the whole step exists for. Eight empty boxes said nothing; one `+`
 * says everything. Rendered at both of its two positions, with Shift held and
 * not, because the footer changes under the modifier now. */
{
  for (const sel of [0, 1]) for (const shift of [false, true]) {
    const decls = componentsAtCap(CAP);
    const { fb, reads } = render(CAP, sel, { shift, empty: true });
    if (fb.clipped() !== 0)
      fail("an empty Master FX at selection " + sel + " clipped " + fb.clipped() + " pixels");
    void reads; void decls;
  }
}

/* ---- 5. no Master FX component wears the synth band -------------------- *
 *
 * chain_diagram paints a filled band across the top of any `kind: "synth"`
 * box -- the landmark the scrolling row leans on. Master FX has no synth, so
 * claiming that kind would put a landmark on an ordinary FX.
 */
{
  const decls = componentsAtCap(CAP);
  const full = decls.makeEmptyMasterFxConfig();
  for (let i = 1; i <= CAP; i++) full["fx" + i].module = "module" + i;
  decls.setConfig(full);
  const comps = decls.masterFxChainComponents();
  const synth = comps.filter((c) => c.kind === "synth");
  if (synth.length) {
    fail("Master FX components claim kind synth: " +
         synth.map((c) => c.key).join(" ") + " - there is no synth here, and " +
         "the band is the landmark that says there is");
  }
  if (comps.some((c) => !c.kind)) {
    fail("a Master FX component has no kind - chain_diagram dispatches on it, " +
         "so leaving it undefined makes the box type accidental");
  }
}

/* ---- 6. the knob card draws, and stays on the display ----------------- *
 *
 * 4b put the slot editors knob card on Master FX. It is a modal drawn OVER the
 * diagram, and the modal is the last thing on the screen, so a card that
 * overhangs would clip silently in exactly the way the fixed box row used to.
 * Rendered at both ends of the row and with a full widget strip.
 *
 * It also has to be REACHED: a card block made unreachable by a typeof guard
 * would pass every clipping assertion in this file while drawing nothing, so
 * the pixel count is compared with the card on and off.
 */
{
  const CARD = { name: "ROOM SIZE", value: "0.62", row: 0, touched: 1,
    page: { kind: "knobs", keys: ["a", "b", "c", "d", null, null, null, null] },
    metaIndex: buildMetaIndex({ hierarchy: null, chainParams: [
      { key: "a", name: "Room", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "b", name: "Damp", type: "float", min: 0, max: 1, step: 0.01 },
      { key: "c", name: "Mode", type: "enum", options: ["Hall", "Room", "Plate"] },
      { key: "d", name: "Mix",  type: "float", min: 0, max: 1, step: 0.01 }] }),
    values: { a: 0.62, b: 0.25, c: 1, d: 0.8 },
    viz: null, modulated: null };
  const plain = render(CAP, 0, {});
  for (const sel of [0, CAP - 1]) {
    const { fb, reads } = render(CAP, sel, { card: CARD });
    if (fb.clipped() !== 0)
      fail("selection " + sel + " with the knob card up drew " + fb.clipped() +
           " pixels OUTSIDE the display");
    if (fb.missingGlyphs.size)
      fail("the knob card asked for glyphs the device font lacks: " +
           [...fb.missingGlyphs].join(""));
    if (reads.length > plain.reads.length)
      fail("the knob card cost " + (reads.length - plain.reads.length) +
           " extra reads on the DRAW path - every value it shows was read on " +
           "touch-down, and a round trip is ~2.8ms against a 1.68ms page render");
  }
  const withCard = render(CAP, 0, { card: CARD });
  if (withCard.fb.countLit() === plain.fb.countLit())
    fail("the screen is pixel-identical with and without the knob card - the " +
         "card block is unreachable, which is how a lift silently measures a " +
         "feature that is switched off");
}

if (failures) process.exit(1);
console.log("PASS: Master FX row fits at cap " + CAP + " (and " + (CAP + 1) +
            "), read budget bounded by the " + CAPACITY + " drawn boxes, and " +
            "the knob card draws over it for free");
'
