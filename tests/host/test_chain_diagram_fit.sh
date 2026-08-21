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
import { drawChainDiagram, layoutChainDiagram, fitAbbrev, BOX_W, BOX_H, GAP, DEFAULT_X, DEFAULT_Y, SYNTH_BAND_H }
  from "./src/shared/chain_diagram.mjs";

/*
 * Where the boxes ACTUALLY start.
 *
 * This test drives a TWO box chain, and a chain shorter than the window is
 * CENTRED rather than left aligned, so DEFAULT_X is the origin of the strip
 * and not the origin of the first box. Asking the layout is the same discipline
 * the ring recovery below already follows: hardcoding where a thing is assumed
 * to be is how this test missed the regression it exists for.
 */
const ORIGIN = layoutChainDiagram([0, 0], 0).boxX(0);

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

/* The device font: 5px glyph plus a 1px gap. */
const W = (s) => String(s).length * 6;

function draw(abbrevs, sel = 0) {
  const prints = [];
  const rects = [];
  const ctx = {
    /* Record the 1px-wide vertical LINES the outline helper emits, so the
     * test recovers where the rings ACTUALLY are rather than where it assumes.
     * Hardcoding the expected inset made this test miss the very regression it
     * exists for: moving the ring moved the collision, and the assertion did
     * not follow it. */
    fillRect: (x, y, w, h) => rects.push({ x, y, w, h }),
    print: (x, y, s) => prints.push({ x, y, s: String(s) }),
    textWidth: W,
  };
  prints.rects = rects;
  const mod = (id) => ({ module: id, params: {} });
  const comps = [
    { key: "synth", kind: "synth",  module: mod("s") },
    { key: "fx1",   kind: "module", module: mod("f") },
  ];
  drawChainDiagram(ctx, comps, sel, { abbrev: (c) => abbrevs[c.key] || "--" });
  return prints;
}

/* ---- 1. the SYNTH band never touches the label ------------------------ *
 *
 * The band replaced an inset RING. The ring cost width on both sides, which is
 * how a 3-char abbrev ("9W9") ended up colliding with the synth landmark. A
 * horizontal band costs no width, so the collision cannot return by
 * arithmetic - but it could return by overlapping vertically, which is what
 * this checks.
 */
for (const selected of [false, true]) {
  const p = draw({ synth: "9W9" }, selected ? 0 : 1);
  const t = p.find((q) => q.s === "9W9");
  const where = selected ? "selected" : "unselected";
  if (!t) {
    fail(where + ": the 3-char synth label was truncated or never drawn: " +
         JSON.stringify(p.map((q) => q.s)));
    continue;
  }
  /* The label occupies 7 rows of the 5x7 face. */
  const tTop = t.y, tBot = t.y + 6;
  const band = p.rects.find((r) => r.h === SYNTH_BAND_H && r.w === BOX_W - 2 &&
                                   r.x >= ORIGIN && r.x < ORIGIN + BOX_W);
  if (!band) {
    fail(where + ": the synth drew no band - it is the only landmark once the " +
         "chain scrolls, and it must survive selection too");
    continue;
  }
  if (band.y + band.h - 1 >= tTop && band.y <= tBot) {
    fail(where + ": the synth band (rows " + band.y + ".." + (band.y + band.h - 1) +
         ") overlaps the label (rows " + tTop + ".." + tBot + ")");
  }
  /* And the band must stay inside its own box. */
  if (band.x < ORIGIN || band.x + band.w > ORIGIN + BOX_W ||
      band.y < DEFAULT_Y || band.y + band.h > DEFAULT_Y + BOX_H) {
    fail(where + ": the synth band is not inside its box");
  }
}

/* ---- 1b. the synth gets the SAME label room as any other box ---------- *
 *
 * The whole point of a horizontal mark. If the synth ever costs width again,
 * the 3-char collision comes back. */
{
  const p = draw({ synth: "9W9", fx1: "9W9" });
  const a = p.filter((q) => q.s === "9W9");
  if (a.length !== 2) fail("expected the label on both boxes, got " + a.length);
  else {
    const offA = a[0].x - ORIGIN;
    const offB = a[1].x - (ORIGIN + BOX_W + GAP);
    if (offA !== offB) {
      fail("the synth label sits at offset " + offA + " but the FX label at " + offB +
           " - the synth is being given different room again");
    }
  }
}

/* ---- 2. a long label never leaves its own box ------------------------- */
{
  const p = draw({ fx1: "STRUCT" });
  const boxL = ORIGIN + BOX_W + GAP;      /* fx1 is the second box */
  const t = p.find((q) => q.s.startsWith("S") && q.x >= ORIGIN + BOX_W);
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
console.log("PASS: chain diagram labels stay inside their boxes, and clear the synth band");
'
