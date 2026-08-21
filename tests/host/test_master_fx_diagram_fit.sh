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
import { readFileSync } from "node:fs";
import { createFramebuffer } from "./tools/param-pages/harness.mjs";
import { drawChainDiagram, DIAGRAM_W, BOX_H as DIAGRAM_BOX_H, CAPACITY }
  from "./src/shared/chain_diagram.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const jsSrc = readFileSync("src/shadow/shadow_ui.js", "utf8");
const mjsSrc = readFileSync("src/shadow/shadow_ui_master_fx.mjs", "utf8");

/* The cap is READ from source, never restated here: a test that hardcodes 8
   stops testing the thing it exists for the moment somebody raises it. */
const capM = jsSrc.match(/^const MASTER_FX_SLOTS = (\d+);/m);
if (!capM) { console.error("FAIL: could not read MASTER_FX_SLOTS"); process.exit(1); }
const CAP = parseInt(capM[1], 10);

/* MASTER_FX_CHAIN_COMPONENTS, evaluated out of shadow_ui.js rather than
   reconstructed -- the list the device draws is the list under test. */
function componentsAtCap(cap) {
  let block = jsSrc.slice(jsSrc.indexOf("const MASTER_FX_SLOTS = "),
                          jsSrc.indexOf("let selectingMasterFxModule"));
  block = block.replace(/const MASTER_FX_SLOTS = \d+;/, "const MASTER_FX_SLOTS = " + cap + ";");
  return new Function(block +
    "return {MASTER_FX_CHAIN_COMPONENTS, makeEmptyMasterFxConfig};")();
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
  /* Every slot loaded: the worst case for both width and read count. */
  for (let i = 1; i <= cap; i++) cfg["fx" + i].module = "module" + i;

  const uictx = {
    masterShowingNamePreview: false, masterConfirmingOverwrite: false,
    masterConfirmingDelete: false, helpDetailScrollState: null, helpNavStack: [],
    inMasterPresetPicker: false, inMasterFxSettingsMenu: false,
    selectingMasterFxModule: false,
    selectedMasterFxComponent: selected,
    masterFxConfig: cfg,
    MASTER_FX_CHAIN_COMPONENTS: decls.MASTER_FX_CHAIN_COMPONENTS,
    MASTER_FX_OPTIONS: [],
    currentMasterPresetName: "",
    getMasterFxParam: (i, k) => { reads.push("master_fx:fx" + (i + 1) + ":" + k); return ""; },
    /* Three characters, the longest a module.json abbrev is seen to be. */
    getModuleAbbrev: (id) => (id ? String(id).slice(0, 3).toUpperCase() : "--"),
    isTextEntryActive: () => false,
    drawTextEntry: () => {}, drawHelpDetail: () => {}, drawHelpList: () => {},
  };

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
    drawChainDiagram, DIAGRAM_W, DIAGRAM_BOX_H,
    SCREEN_WIDTH: 128,
    truncateText: (t, n) => String(t == null ? "" : t).slice(0, n),
    drawMasterNamePreview: () => {}, drawMasterConfirmOverwrite: () => {},
    drawMasterConfirmDelete: () => {}, drawMasterPresetPicker: () => {},
    drawMasterFxSettingsMenu: () => {}, drawMasterFxModuleSelect: () => {},
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
const SELECTIONS = [-1, 0, 1, Math.floor(CAP / 2), CAP - 1, CAP];
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

/* ---- 4. the settings box is never treated as an FX slot ---------------- */
{
  const { reads } = render(CAP, CAP, {});
  if (reads.some((k) => k.indexOf("master_fx:settings") === 0)) {
    fail("the settings box was asked for a slot parameter: " +
         reads.filter((k) => k.indexOf("master_fx:settings") === 0).join(" "));
  }
}

/* ---- 5. no Master FX component wears the synth band -------------------- *
 *
 * chain_diagram paints a filled band across the top of any `kind: "synth"`
 * box -- the landmark the scrolling row leans on. Master FX has no synth, so
 * claiming that kind would put a landmark on an ordinary FX.
 */
{
  const comps = componentsAtCap(CAP).MASTER_FX_CHAIN_COMPONENTS;
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

if (failures) process.exit(1);
console.log("PASS: Master FX row fits at cap " + CAP + " (and " + (CAP + 1) +
            "), read budget bounded by the " + CAPACITY + " drawn boxes");
'
