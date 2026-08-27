#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A DOOR SWALLOWED BY A PICTURE STILL HAS TO LOOK LIKE A DOOR.
#
# An opaque cell (filepath, canvas, string) is normally drawn by drawOpaqueBox,
# which since SCH-50 draws its own notched frame with a chevron in the broken
# edge. That is why such a cell is EXCLUDED from the corner brackets: the
# brackets would sit one pixel outside a frame that already exists and read as
# a doubled border rather than as a mark.
#
# But a viz group SUPPRESSES that widget -- the sample waveform is drawn across
# the whole span and the per-cell widget is skipped. So on granny "Main - 2",
# where `sample_path` sits inside the waveform between `position` and `spray`,
# the exclusion deferred to a frame that was never drawn: no frame, no chevron,
# no brackets. Nothing at all said the middle of that strip was a door, and the
# only marks anywhere near it were the spray fences. Reported from the device as
# "empty sample selection is indistinguishable from the spray control".
#
# So the exclusion is void on a COVERED cell. Asserted on PIXELS, in the cell
# rect, because neither the presence nor the absence of the brackets is visible
# in a code review of the draw call -- the condition reads plausibly either way.
#
# The RESULT is asserted, not the condition: a test that grepped for
# `covered[col]` would pass with the term inverted.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
import { renderPageMovy } from "./src/shared/param_pages/render_page_movy.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { resolveViz } from "./src/shared/param_pages/viz.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "  ok  " : "  FAIL ") + m); if (!c) fail++; };

/* granny in miniature: a playback cursor, its spray, and the file all three
 * describe -- which is what makes detectSample span the row and cover the
 * filepath cell. */
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

/* A real framebuffer, and colour 0 must ERASE.
 *
 * A write-only ink LIST looks equivalent and is not: notchCorners knocks the
 * four corners back out with fillRect(..., 0), and the notch is the ONLY thing
 * that separates the opaque widget frame from the brackets -- they occupy the
 * identical rect (drawOpaqueBox: cellX + 1, ky, CELL_W - 2, BOX_H). Ignoring
 * clears makes a frame and a frame-plus-brackets read the same, which is
 * exactly the double-marking regression this file exists to catch. */
function render(viz) {
  const lit = new Set();
  const ctx = {
    fillRect: (x, y, w, h, c) => {
      for (let yy = y; yy < y + h; yy++)
        for (let xx = x; xx < x + w; xx++) {
          if (c === 0) lit.delete(xx + "," + yy); else lit.add(xx + "," + yy);
        }
    },
    print: () => {},
    drawText: () => {},
    measureText: (s) => String(s).length * 4,
    clear: () => {},
  };
  renderPageMovy(ctx, { page, values: { position: "0.5", spray: "0.2",
                                        sample_path: "", size: "0.5" },
                        metaIndex, held: [], viz });
  return lit;
}

/* The EXACT bracket pixels for a cell, mirroring drawDivableMark:
 *   drawBrackets(cellLeft + 1, rowY, cellW - 2, BOX_H)
 * All 24 of them, not a corner region. A region test cannot tell brackets from
 * the picture: a spray fence is a full-height column, so it lights the top and
 * the bottom of whatever cell it lands in and reads as two corners. Requiring
 * every arm of all four corners is what only the mark can satisfy. */
const BRACKET_LEN = 4;
function bracketPixels(col) {
  const x = col * CELL_W + 1, y = ROW0_Y, w = CELL_W - 2, h = BOX_H;
  const px = [];
  for (let i = 0; i < BRACKET_LEN; i++) {
    px.push([x + i, y], [x + w - 1 - i, y], [x + i, y + h - 1], [x + w - 1 - i, y + h - 1]);
  }
  for (let i = 0; i < BRACKET_LEN - 1; i++) {
    px.push([x, y + i], [x + w - 1, y + i], [x, y + h - 1 - i], [x + w - 1, y + h - 1 - i]);
  }
  return px;
}
/* ...and brackets are a SUBSET of a full frame drawn on the SAME rect, so "all
 * 24 lit" alone cannot tell the mark from the opaque widget frame it defers to.
 * Two discriminators, each aimed at one direction of the mistake:
 *
 *   BRACKETS: all 24 lit, and the MIDDLE of the top edge dark. A continuous
 *             frame has that middle; corner stubs by definition do not.
 *   FRAME:    that middle lit, and all four CORNERS dark -- notchCorners
 *             knocks them out. Drawing brackets over a frame fills the notch
 *             back in, which is the doubled border, so this is what catches
 *             "mark every opaque cell".
 */
const cornerPixels = (col) => {
  const x = col * CELL_W + 1, y = ROW0_Y, w = CELL_W - 2, h = BOX_H;
  return [[x, y], [x + w - 1, y], [x, y + h - 1], [x + w - 1, y + h - 1]];
};
const topMiddle = (col) => [col * CELL_W + Math.floor(CELL_W / 2), ROW0_Y];
const on = (lit, [x, y]) => lit.has(x + "," + y);

function hasBrackets(lit, col) {
  return bracketPixels(col).every((p) => on(lit, p)) && !on(lit, topMiddle(col));
}
function hasNotchedFrame(lit, col) {
  return on(lit, topMiddle(col)) && cornerPixels(col).every((p) => !on(lit, p));
}

const { groups } = resolveViz({ keys: KEYS, metaIndex });
const covering = groups.filter((g) => g.slotStart <= 2 && g.slotStart + g.slotSpan > 2);
ok(covering.length === 1,
   "the fixture reproduces the real shape: a viz group covers the filepath cell");

const withViz = render(groups);

/* THE assertion. Slot 2 is the filepath, swallowed by the waveform. */
ok(hasBrackets(withViz, 2),
   "a COVERED filepath cell is bracketed -- it is a door and the mark is the only thing left saying so");

/* The cursor cell keeps the brackets it always had: a ranged wav_position is
 * divable_mark without being KIND_OPAQUE, which is the whole reason the two
 * flags are separate. If this went out with the change, the fix moved the mark
 * instead of adding one. */
ok(hasBrackets(withViz, 0),
   "the position cell KEEPS its brackets -- the covered case adds a mark, it does not move one");

/* Its neighbour is an ordinary float. It must NOT be marked, or the brackets
 * stop meaning anything: marking every cell is the same as marking none. */
ok(!hasBrackets(withViz, 1),
   "the spray cell between them is NOT bracketed -- a plain float opens nothing");

/* And the exclusion still holds where it was aimed. Uncovered, the filepath
 * draws its own notched frame and must NOT also wear the brackets, or the two
 * sit one pixel apart and read as a doubled border. Built by dropping viz
 * entirely, so nothing is covered. */
const noViz = render([]);
ok(hasNotchedFrame(noViz, 2),
   "an UNCOVERED opaque cell keeps its NOTCHED frame and nothing else -- brackets over it fill the notch back in and read as a doubled border");

if (fail) { console.log("FAIL: " + fail + " assertion(s)"); process.exit(1); }
console.log("PASS: a door swallowed by a picture is still marked");
'
