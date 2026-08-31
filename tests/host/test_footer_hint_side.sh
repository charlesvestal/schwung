#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# drawFooter(ctx, hints, { backLeft: true }) moves the BACK hint to the LEFT
# edge, with the remaining hints flowing after it -- the mirror image of the
# default arrangement, not a variant of it.
#
# TB-3PO wants a compressed step lane in the RIGHT half of the footer band,
# keeping one hint pill on the left. The default drawFooter PINS BACK to the
# right edge (see test_footer_back_pinned_right.sh), which is exactly where
# the lane wants to sit -- so a caller needs a way to say "BACK goes left,
# something else owns the right".
#
# Default behaviour must be untouched: every shipped screen calls drawFooter
# with no third argument and depends on BACK staying pinned right.
#
# NO APOSTROPHES inside the node script: single-quoted bash strings.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node --input-type=module -e '
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { drawFooter, hintPairWidth } from "./src/shared/param_pages/render_page_movy.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const PILL_H = 7; /* FONT4_HEIGHT + 2, pinned separately in test_footer_back_pinned_right.sh */

/* Record only the PILL rects, same technique as the sibling test: every hint
 * draws exactly one pill of a known height, so filtering on that height
 * separates pills from glyph pixels without knowing how the font renders. */
function pillsCtx() {
  const xs = [];
  const ctx = {
    fillRect: (x, y, w, h) => { if (h === PILL_H) xs.push({ x, w }); },
    print: () => {},
    textWidth: (s) => s.length * 6,
  };
  return { ctx, xs };
}

const hints = [["JOG", "SEL"], ["BACK", "EXIT"]];

/* ---- 1. backLeft moves the BACK pill leftmost column, default keeps it ---- *
 *
 * "leftmost lit column of the BACK pill" is exactly the pill x recorded by
 * fillRect for that pair, since a pill is drawn as one solid fillRect call. */
{
  const def = pillsCtx();
  const drawnDefault = drawFooter(def.ctx, hints);
  const left = pillsCtx();
  const drawnLeft = drawFooter(left.ctx, hints, { backLeft: true });

  if (drawnDefault !== hints.length) fail("default: drew " + drawnDefault + " of " + hints.length);
  if (drawnLeft !== hints.length) fail("backLeft: drew " + drawnLeft + " of " + hints.length);

  if (def.xs.length !== 2 || left.xs.length !== 2) {
    fail("expected exactly 2 pills in each arrangement");
  } else {
    const defBackX = def.xs[def.xs.length - 1].x;   /* default: BACK drawn last, pinned right */
    const leftBackX = left.xs[0].x;                 /* backLeft: BACK drawn first, at the left edge */

    if (leftBackX !== 1) {
      fail("backLeft: BACK pill x=" + leftBackX + ", want 1 (the left edge)");
    }
    if (defBackX === leftBackX) {
      fail("BACK pill sits at the same x (" + defBackX + ") in both arrangements " +
           "-- backLeft did not move anything");
    }
    if (leftBackX >= defBackX) {
      fail("backLeft: BACK pill at x=" + leftBackX + " is not left of the default " +
           "arrangement at x=" + defBackX);
    }

    /* The flow hint (JOG SEL) must come AFTER BACK in backLeft mode, not
     * before it -- BACK owns the left edge, the rest follows. */
    const leftFlowX = left.xs[1].x;
    if (leftFlowX <= leftBackX) {
      fail("backLeft: the flow hint at x=" + leftFlowX +
           " does not sit after the BACK pill at x=" + leftBackX);
    }
  }
}

/* ---- 2. Default arrangement is byte-identical to before this change ---- *
 *
 * Pinned as an exact pixel snapshot of the footer band, rendered through the
 * real device font atlas via the harness -- not merely "BACK is on the
 * right", which the sibling test already covers, but every pixel. */
{
  const fb = createFramebuffer();
  drawFooter(drawContext(fb), hints);

  const EXPECTED = [
    "..################...................................................................#####################......................",
    ".####..##..###...##...###.####.#....................................................##...###..###...#.##.##..####.#..#.#.###....",
    ".#####.#.##.#.#####..#....#....#....................................................##.##.#.##.#.####.#.###..#....#..#.#..#.....",
    ".#####.#.##.#.#..##...##..###..#....................................................##...##....#.####..####..###...##..#..#.....",
    ".##.##.#.##.#.##.##.....#.#....#....................................................##.##.#.##.#.####.#.###..#....#..#.#..#.....",
    ".###..###..###...##..###..####.####.................................................##...##.##.##...#.##.##..####.#..#.#..#.....",
    "..################...................................................................#####################......................",
  ].join("\n");

  let minY = -1, maxY = -1;
  for (let y = 0; y < fb.height; y++) {
    let any = false;
    for (let x = 0; x < fb.width; x++) if (fb.pixels[y * fb.width + x]) { any = true; break; }
    if (any) { if (minY < 0) minY = y; maxY = y; }
  }
  const lines = [];
  for (let y = minY; y <= maxY; y++) {
    let line = "";
    for (let x = 0; x < fb.width; x++) line += fb.pixels[y * fb.width + x] ? "#" : ".";
    lines.push(line);
  }
  const got = lines.join("\n");

  if (got !== EXPECTED) {
    fail("default footer pixels changed -- this is the regression this test " +
         "exists to catch, NOT something to update the snapshot for");
    console.error("--- got ---\n" + got);
    console.error("--- want ---\n" + EXPECTED);
  }

  if (fb.clipped() !== 0) fail("default: " + fb.clipped() + " pixels drawn off-screen");
}

/* ---- 3. Nothing clips in the backLeft arrangement either ---- */
{
  const fb = createFramebuffer();
  drawFooter(drawContext(fb), hints, { backLeft: true });
  if (fb.clipped() !== 0) fail("backLeft: " + fb.clipped() + " pixels drawn off-screen");
  if (fb.countLit() === 0) fail("backLeft: nothing was drawn");
}

/* ---- 4. backLeft with no BACK hint present falls through harmlessly ---- */
{
  const noBack = [["JOG", "SEL"], ["CLK", "OPEN"]];
  const a = pillsCtx();
  const drawnA = drawFooter(a.ctx, noBack);
  const b = pillsCtx();
  const drawnB = drawFooter(b.ctx, noBack, { backLeft: true });
  if (drawnA !== drawnB || JSON.stringify(a.xs) !== JSON.stringify(b.xs)) {
    fail("backLeft with no BACK hint should behave exactly like the default path");
  }
}

if (failures) process.exit(1);
console.log("PASS: drawFooter({ backLeft: true }) moves BACK left; default arrangement is pixel-identical");
'
