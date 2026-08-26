#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# OCTAVE AND VOICE COUNT ARE NUMBERS, NOT POSITIONS.
#
# An arc answers "how far along its range is this", which is the right question
# for a cutoff and the wrong one for an octave offset: -1 and +1 are two detents
# apart on a -24..24 transpose and read as two nearly identical arcs. For a
# small integer the number IS the value, so draw the number.
#
# THE SIGN IS THE POINT, and only on a range that HAS a negative side. A framed
# "2" where "+2" was meant still renders, still fits, and is wrong in the one
# way the whole cell exists to fix. A "+4" on a 1..8 voice count is noise --
# there is nothing for it to contrast with.
#
# NO APOSTROPHES inside the node scripts: single-quoted bash strings.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the framed number tests" >&2
  exit 1
fi

node --input-type=module -e '
import { shouldDrawBigNumber, bigNumberText, BIG_NUM_MAX_SPAN, BIG_NUM_BIPOLAR_MAX_SPAN }
  from "./src/shared/param_pages/render_page_movy.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };
const meta = (o) => Object.assign({ kind: "number" }, o);

/* -------------------------------------------------------------- what frames */
ok(shouldDrawBigNumber(meta({ type: "int", min: -24, max: 24 })),
   "a bipolar int (transpose) draws big");
ok(shouldDrawBigNumber(meta({ type: "int", min: 1, max: 8 })),
   "a unipolar int (voice count) draws big");
ok(shouldDrawBigNumber(meta({ type: "int", min: -2, max: 2 })),
   "a small octave offset draws big");
ok(!shouldDrawBigNumber(meta({ type: "float", min: -1, max: 1 })),
   "a float never draws big -- an arc is right for a continuous value");
ok(!shouldDrawBigNumber(meta({ type: "int", min: 0, max: 20000 })),
   "a wide int (a frequency in Hz) does not draw big -- it would not fit, and an "
   + "arc is the honest picture of a big range");
ok(!shouldDrawBigNumber(meta({ type: "enum", kind: "enum", options: ["a", "b"] })),
   "an enum does not draw big -- it has its own square");
ok(!shouldDrawBigNumber(null), "no meta does not draw big");
ok(!shouldDrawBigNumber(meta({ type: "int" })),
   "an int with no declared range does not draw big -- nothing bounds the digits");
ok(shouldDrawBigNumber(meta({ type: "int", min: 0, max: BIG_NUM_MAX_SPAN })),
   "exactly at the unipolar span limit still draws big");
ok(!shouldDrawBigNumber(meta({ type: "int", min: 0, max: BIG_NUM_MAX_SPAN + 1 })),
   "one past the unipolar span limit does not");
ok(shouldDrawBigNumber(meta({ type: "int", min: -BIG_NUM_BIPOLAR_MAX_SPAN / 2,
                            max: BIG_NUM_BIPOLAR_MAX_SPAN / 2 })),
   "a bipolar range gets a wider bound -- the SIGN is information an arc "
   + "cannot show, so it earns the room");
ok(!shouldDrawBigNumber(meta({ type: "int", min: -BIG_NUM_BIPOLAR_MAX_SPAN,
                             max: BIG_NUM_BIPOLAR_MAX_SPAN })),
   "but not unlimited room");

/* THE AMOUNTS. These are typed int and are NOT numbers you read -- they are
   sweeps, and an arc is the right picture. Drawing them big was the first version
   of this rule, and it drew 1392 params across 60 modules. */
ok(!shouldDrawBigNumber(meta({ type: "int", min: 0, max: 100 })),
   "a 0..100 amount (obxd volume) does not draw big");
ok(!shouldDrawBigNumber(meta({ type: "int", min: 0, max: 127 })),
   "a 0..127 amount (9w9 tune, minijv macro_cutoff) does not draw big");

/* THE TARGETS, spelled as the fleet actually declares them. */
ok(shouldDrawBigNumber(meta({ type: "int", min: -4, max: 4 })),
   "sfz/sf2 octave_transpose draws big");
ok(shouldDrawBigNumber(meta({ type: "int", min: -24, max: 24 })),
   "hush1 transpose draws big");
ok(shouldDrawBigNumber(meta({ type: "int", min: 1, max: 8 })),
   "a voice count draws big");
ok(shouldDrawBigNumber(meta({ type: "int", min: 1, max: 16 })),
   "a MIDI channel draws big");

/* --------------------------------------------------------------- the string */
ok(bigNumberText(meta({ type: "int", min: -24, max: 24 }), "2") === "+2",
   "a positive value on a bipolar range shows its +");
ok(bigNumberText(meta({ type: "int", min: -24, max: 24 }), "-1") === "-1",
   "a negative value shows its -");
ok(bigNumberText(meta({ type: "int", min: -24, max: 24 }), "0") === "0",
   "zero carries no sign");
ok(bigNumberText(meta({ type: "int", min: 1, max: 8 }), "4") === "4",
   "a unipolar range carries no + -- there is nothing to contrast with");
ok(bigNumberText(meta({ type: "int", min: -24, max: 24 }), null) === "--",
   "an unread value is --, not 0");
ok(bigNumberText(meta({ type: "int", min: -24, max: 24 }), "") === "--",
   "an unserved value is --, not 0 -- see the tri-state read contract");
ok(bigNumberText(meta({ type: "int", min: -24, max: 24 }), "abc") === "--",
   "a non-numeric answer is --, not 0");

process.exit(fail ? 1 : 0);
'

node --input-type=module -e '
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { drawBigNumber, BOX_H } from "./src/shared/param_pages/render_page_movy.mjs";
import { fontWidth, HEIGHT } from "./src/shared/param_pages/font_tamzen6x12.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const CX = 40, Y = 20;
const fb = createFramebuffer();
drawBigNumber(drawContext(fb), CX, Y, "+2");

/* NO FRAME. The box is the ENUM affordance -- an enum that declares options is
   divable, and the square plus its corner brackets are what say a list is
   behind this cell. A small int has no list and can never have one, so a box
   here advertised a door that does not open. */
const rowRun = (y) => {
  let run = 0, best = 0;
  for (let x = 0; x < fb.width; x++) {
    run = fb.pixels[y * fb.width + x] ? run + 1 : 0;
    if (run > best) best = run;
  }
  return best;
};
let framed = false;
for (let y = Y; y < Y + BOX_H; y++) if (rowRun(y) >= 14) framed = true;
ok(!framed, "no horizontal rule is drawn -- this cell has no frame");

/* IT IS THE BIG FONT, AND IT IS BIGGER THAN THE BODY FONT.
   This used to bound the glyphs at the device font height, on the reasoning
   that dropping the box promoted the value from the 4x5 enum font to the 6x7
   body font. Unframed at 7 rows the cell then read as BARE, so it now takes the
   Tamzen 8x16 cut and the upper bound is the WIDGET BOX -- which is what
   actually constrains it. The lower bound moves with it: no taller than the
   body font would now be a regression, not a limit. */
let top = -1, bot = -1;
for (let y = 0; y < fb.height; y++)
  for (let x = 0; x < fb.width; x++)
    if (fb.pixels[y * fb.width + x]) { if (top < 0) top = y; bot = y; }
ok(top >= 0, "something was drawn");
ok(bot - top + 1 > HEIGHT,
   "the glyphs are taller than the " + HEIGHT + "-row body font, got " +
   (bot - top + 1) + " rows -- otherwise the cell is bare again");
ok(bot - top + 1 <= BOX_H,
   "and inside the widget box, got " + (bot - top + 1) + " of " + BOX_H);

/* CENTRED ON THE CELL, not left-aligned at it -- the width changes with the
   sign and the digit count, so only drawBigNumber can place it. */
let lx = -1, rx = -1;
for (let x = 0; x < fb.width; x++)
  for (let y = 0; y < fb.height; y++)
    if (fb.pixels[y * fb.width + x]) { if (lx < 0) lx = x; rx = x; }
ok(Math.abs(((lx + rx) / 2) - CX) <= 1.5,
   "the text is centred on the cell centre, got " + ((lx + rx) / 2) + " vs " + CX);

/* THE SIGN IS DRAWN, not merely returned by bigNumberText. */
const bare = createFramebuffer();
drawBigNumber(drawContext(bare), CX, Y, "2");
ok(fb.countLit() > bare.countLit(), "the + reaches the framebuffer");
ok(fontWidth("+2") > fontWidth("2"), "and it costs real width");

ok(fb.clipped() === 0, "nothing was drawn off-screen");

process.exit(fail ? 1 : 0);
'

node --input-type=module -e '
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import * as RM from "./src/shared/param_pages/render_page_movy.mjs";
import { HEIGHT as NUM_H } from "./src/shared/param_pages/font_big_num.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };
const at = (fb, x, y) => fb.pixels[y * fb.width + x];

/*
 * THE WIRING, through the real renderer.
 *
 * The assertions above exercise shouldDrawBigNumber, bigNumberText and
 * drawFramedNumber in isolation -- all three can be perfect while
 * drawKnobWidget never calls them, and the unit tests stay green. Only the
 * geometry snapshot would notice, and a snapshot says "something moved", not
 * "this is what the cell is FOR".
 */
const render = (meta, value) => {
  const fb = createFramebuffer();
  RM.renderPageMovy(drawContext(fb), {
    page: { kind: "knobs", keys: ["p", null, null, null, null, null, null, null] },
    metaIndex: { getOrGuess: () => meta },
    values: { p: value },
    title: "T", pageIndex: 0, pageCount: 1, viz: [],
  });
  return fb;
};

const oct = { key: "p", kind: "number", type: "int", min: -4, max: 4, name: "Oct" };
const amt = { key: "p", kind: "number", type: "int", min: 0, max: 127, name: "Amt" };

const framed = render(oct, "2");
const arced  = render(amt, "64");

/*
 * An ARC is a ring: it lights pixels in the widget rows well away from the
 * cell centre line. A big number is glyphs on one text baseline, centred in the
 * box, so the rows ABOVE it are empty.
 *
 * The band is DERIVED from the font height, not fixed at three rows. It was
 * three, which encoded "a 7px glyph centred in a 15px box leaves 4 clear rows"
 * -- a proxy that silently became wrong the moment the cell took a taller face,
 * and failed as though the number had grown a ring. What is actually being
 * asserted is that the number does not reach where the arc does, so the empty
 * band has to move with the glyphs.
 */
const CLEAR_ROWS = Math.floor((RM.BOX_H - NUM_H) / 2);
const upperLit = (fb) => {
  let n = 0;
  for (let y = RM.ROW0_Y; y < RM.ROW0_Y + CLEAR_ROWS; y++)
    for (let x = 0; x < RM.CELL_W; x++) if (at(fb, x, y) === 1) n++;
  return n;
};
ok(upperLit(arced) > 0,
   "a 0..127 amount keeps its ARC, which reaches the top of the cell");
ok(upperLit(framed) === 0,
   "a small bipolar int does NOT -- it is one line of glyphs, not a ring "
   + "(got " + upperLit(framed) + " lit)");
/*
 * Non-empty, and confined to its own glyph band.
 *
 * This used to require FEWER pixels than the arc, which was never the property
 * being defended -- it was an accident of a 7px, 1px-stem face. A 2px-stem
 * 11-row number legitimately carries more ink than a thin ring (84 against 56),
 * so that clause started failing on a change that made the cell better.
 *
 * What actually separates the two is EXTENT, not quantity: the number occupies
 * one text band, the arc spans the box.
 */
/* Bounded to the WIDGET BOX. `render` draws a whole page, so an unbounded walk
 * measures the header down to the label row -- 29 rows for both cells, which
 * says nothing about either. */
const vExtent = (fb) => {
  let lo = -1, hi = -1;
  for (let y = RM.ROW0_Y; y < RM.ROW0_Y + RM.BOX_H; y++)
    for (let x = 0; x < RM.CELL_W; x++)
      if (at(fb, x, y) === 1) { if (lo < 0) lo = y; hi = y; break; }
  return lo < 0 ? 0 : hi - lo + 1;
};
ok(framed.countLit() > 0, "and it really drew something");
ok(vExtent(framed) <= NUM_H,
   "the number is one band of glyphs, got " + vExtent(framed) + " rows for a " +
   NUM_H + "-row font");
ok(vExtent(arced) > NUM_H,
   "the arc spans more than a text band, got " + vExtent(arced) + " rows");

process.exit(fail ? 1 : 0);
'
