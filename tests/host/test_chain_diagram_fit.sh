#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Box labels must stay inside their boxes.
#
# Two hardware reports, one cause: the abbrev was centred with no fit and no
# clamp.
#
#   "the thick border of the synth module doesnt really work with 3 letter
#    shortcodes like 9W9"  -- the synth wears a second ring, and an even 2px
#    inset left 16px inside it while a 3-char label is ~17px, so the label sat
#    on its own landmark.
#
#   "some modules like structure have longer shortcodes that break the box"
#    -- abbrevs come from module.json, so their length is a third party choice.
#    Anything wider than the 22px box gave a NEGATIVE centring offset: the
#    label started outside its own box and ran over the neighbour.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node --input-type=module -e '
import { drawChainDiagram, fitAbbrev, BOX_W, GAP, DEFAULT_X }
  from "./src/shared/chain_diagram.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* The device font: 5px glyph plus a 1px gap. */
const W = (s) => String(s).length * 6;

function draw(abbrevs) {
  const prints = [];
  const pixels = [];
  const ctx = {
    /* Record the 1px-wide vertical LINES the outline helper emits, so the
     * test recovers where the rings ACTUALLY are rather than where it assumes.
     * Hardcoding the expected inset made this test miss the very regression it
     * exists for: moving the ring moved the collision, and the assertion did
     * not follow it. */
    fillRect: (x, y, w, h) => { if (w === 1) pixels.push({ x, y0: y, y1: y + h - 1 }); },
    print: (x, y, s) => prints.push({ x, y, s: String(s) }),
    textWidth: W,
  };
  prints.pixels = pixels;
  const mod = (id) => ({ module: id, params: {} });
  const comps = [
    { key: "synth", kind: "synth",  module: mod("s") },
    { key: "fx1",   kind: "module", module: mod("f") },
  ];
  drawChainDiagram(ctx, comps, 0, { abbrev: (c) => abbrevs[c.key] || "--" });
  return prints;
}

/* ---- 1. a 3-char label on the SYNTH clears its inner ring -------------- */
{
  const p = draw({ synth: "9W9" });
  const t = p.find((q) => q.s === "9W9");
  if (!t) {
    fail("the 3-char synth label was truncated or never drawn: " +
         JSON.stringify(p.map((q) => q.s)));
  } else {
    /*
     * Find the rings from what was DRAWN, on the row the label sits on. The
     * outline helper emits 1px-wide vertical LINES, so a line at column x
     * spanning rows y0..y1 occupies that column on the label row when the row
     * falls inside its span. Whatever vertical lines the synth box draws, the
     * label must not land on them.
     * Deriving this means the test follows the ring if the inset changes,
     * which is exactly the regression being guarded.
     */
    const cols = p.pixels
      .filter((q) => t.y >= q.y0 && t.y <= q.y1 &&
                     q.x >= DEFAULT_X && q.x < DEFAULT_X + BOX_W)
      .map((q) => q.x)
      .sort((a, b) => a - b);
    const span = { lo: t.x, hi: t.x + W(t.s) - 1 };
    const hit = cols.filter((c) => c >= span.lo && c <= span.hi);
    if (hit.length) {
      fail("the synth label (x " + span.lo + ".." + span.hi + ") lands on box/ring " +
           "pixels at " + JSON.stringify(hit) + " - it collides with its own landmark. " +
           "Vertical lines on that row: " + JSON.stringify(cols));
    }
    if (cols.length < 2) {
      fail("expected the synth box to draw at least an outer and an inner vertical " +
           "line on the label row; found " + JSON.stringify(cols) +
           " - the test can no longer see the ring it is checking against");
    }
  }
}

/* ---- 2. a long label never leaves its own box ------------------------- */
{
  const p = draw({ fx1: "STRUCT" });
  const boxL = DEFAULT_X + BOX_W + GAP;      /* fx1 is the second box */
  const t = p.find((q) => q.s.startsWith("S") && q.x >= DEFAULT_X + BOX_W);
  if (!t) {
    fail("the long label was not drawn: " + JSON.stringify(p.map((q) => [q.x, q.s])));
  } else {
    if (t.x < boxL) {
      fail("long label starts at " + t.x + ", LEFT of its box at " + boxL +
           " - it spills onto the neighbouring box");
    }
    if (t.x + W(t.s) > boxL + BOX_W) {
      fail("long label runs past the right edge of its box");
    }
    if (t.s === "STRUCT") {
      fail("a 6-char label was drawn unfitted - 36px of text in a 22px box");
    }
  }
}

/* ---- 3. truncation always keeps a character --------------------------- *
 *
 * A blank box says the position is EMPTY, which is a different and worse lie
 * than a clipped label. */
{
  const ctx = { textWidth: W };
  if (fitAbbrev(ctx, "ABC", 1) !== "A") fail("fitAbbrev emptied the label");
  if (fitAbbrev(ctx, "AB", 100) !== "AB") fail("fitAbbrev truncated a label that fits");
  if (fitAbbrev(ctx, "", 20) !== "") fail("fitAbbrev invented a character");
}

/* ---- 4. the ordinary case did not regress ----------------------------- */
{
  const p = draw({ synth: "SF", fx1: "RV" });
  if (!p.find((q) => q.s === "SF")) fail("a 2-char synth label regressed");
  if (!p.find((q) => q.s === "RV")) fail("a 2-char FX label regressed");
}

if (failures) process.exit(1);
console.log("PASS: chain diagram labels stay inside their boxes, and clear the synth ring");
'
