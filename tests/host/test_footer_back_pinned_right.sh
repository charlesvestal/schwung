#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# BACK lives at the bottom-RIGHT. Always, whatever else is in the footer.
#
# The rule is a fixed place the user can learn, so it must not drift with the
# number of hints beside it. Being LAST IN THE ARRAY is not the same thing:
# hints pack left-to-right from x=1, so "JOG MOVE / BACK EXIT" put BACK in the
# middle of the screen with ~30px of empty space to its right. That is what the
# chain editor showed while Shift was held, with BACK correctly last in its
# array the whole time. An audit of array ORDER reported zero violations.
#
# The second half of the rule: BACK is the LAST hint dropped, not the first.
# The footer drops what does not fit, and BACK used to be the tail, so a narrow
# footer lost the one hint that should never go missing.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node --input-type=module -e '
const M = await import("./src/shared/param_pages/render_page_movy.mjs");
const W = 128;
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/*
 * Record only the PILL rects. Every hint draws exactly one pill of a known
 * height, so filtering on that height separates pills from glyph pixels
 * without the test needing to know how the font renders.
 */
const PILL_H = M.FONT4_HEIGHT !== undefined ? M.FONT4_HEIGHT + 2 : 7;
function pills(hints) {
  const xs = [];
  const ctx = {
    fillRect: (x, y, w, h) => { if (h === PILL_H) xs.push({ x, w }); },
    print: () => {},
    textWidth: (s) => s.length * 6,
  };
  const drawn = M.drawFooter(ctx, hints);
  return { drawn, xs };
}

const backW = M.hintPairWidth("BACK", "EXIT");
const wantBackX = W - backW;

/* ---- 1. BACK sits at the right edge regardless of what precedes it ---- */
const cases = [
  ["back alone",       [["BACK", "EXIT"]]],
  ["two pairs",        [["JOG", "MOVE"], ["BACK", "EXIT"]]],
  ["three pairs",      [["JOG", "SEL"], ["CLK", "OPEN"], ["BACK", "EXIT"]]],
];
for (const [name, hints] of cases) {
  const { drawn, xs } = pills(hints);
  if (drawn !== hints.length) fail(name + ": drew " + drawn + " of " + hints.length);
  if (!xs.length) { fail(name + ": nothing drawn"); continue; }
  const last = xs[xs.length - 1];
  if (last.x !== wantBackX) {
    fail(name + ": BACK pill at x=" + last.x + ", want " + wantBackX +
         " (pinned to the right edge, not merely last)");
  }
}

/* ---- 2. BACK survives a footer too narrow for everything -------------- *
 *
 * The middle hints are what lose. Before pinning, BACK was the tail and so
 * was the FIRST thing dropped. */
{
  const crowded = [
    ["JOG", "SCRUB"], ["SHFT", "SECT"], ["MUTE", "DFLT"], ["BACK", "EXIT"],
  ];
  const { xs } = pills(crowded);
  if (!xs.length) fail("crowded: nothing drawn at all");
  const last = xs[xs.length - 1];
  if (last.x !== wantBackX) {
    fail("crowded: BACK was dropped or moved (last pill x=" + last.x +
         ", want " + wantBackX + ") - BACK must be the LAST hint dropped");
  }
  if (xs.length === crowded.length) {
    fail("crowded: expected this set to overflow, so the test is not " +
         "exercising the drop path - pick wider words");
  }
}

/* ---- 3. No hint may overlap the pinned BACK --------------------------- */
{
  const { xs } = pills([["JOG", "SCRUB"], ["SHFT", "SECT"], ["BACK", "EXIT"]]);
  for (let i = 0; i < xs.length - 1; i++) {
    if (xs[i].x + xs[i].w > wantBackX) {
      fail("a flow hint at x=" + xs[i].x + " runs into the pinned BACK at " + wantBackX);
    }
  }
}

/* ---- 4. The predicate is about the KEY, not the action ---------------- */
if (!M.isBackHint(["BACK", "EXIT"])) fail("isBackHint missed [BACK, EXIT]");
if (!M.isBackHint(["back", "out"]))  fail("isBackHint is case sensitive");
if (M.isBackHint(["CLK", "BACK"]))   fail("isBackHint matched the ACTION - only the KEY names the button");
if (M.isBackHint(["BACKUP", "GO"]))  fail("isBackHint matched BACKUP - it must be the whole word");

if (failures) process.exit(1);
console.log("PASS: footer BACK is pinned to the right edge and is the last hint dropped");
'
