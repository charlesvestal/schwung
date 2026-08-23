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
import { shouldFrameNumber, framedNumberText, FRAME_MAX_SPAN, FRAME_BIPOLAR_MAX_SPAN }
  from "./src/shared/param_pages/render_page_movy.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };
const meta = (o) => Object.assign({ kind: "number" }, o);

/* -------------------------------------------------------------- what frames */
ok(shouldFrameNumber(meta({ type: "int", min: -24, max: 24 })),
   "a bipolar int (transpose) frames");
ok(shouldFrameNumber(meta({ type: "int", min: 1, max: 8 })),
   "a unipolar int (voice count) frames");
ok(shouldFrameNumber(meta({ type: "int", min: -2, max: 2 })),
   "a small octave offset frames");
ok(!shouldFrameNumber(meta({ type: "float", min: -1, max: 1 })),
   "a float never frames -- an arc is right for a continuous value");
ok(!shouldFrameNumber(meta({ type: "int", min: 0, max: 20000 })),
   "a wide int (a frequency in Hz) does not frame -- it would not fit, and an "
   + "arc is the honest picture of a big range");
ok(!shouldFrameNumber(meta({ type: "enum", kind: "enum", options: ["a", "b"] })),
   "an enum does not frame -- it has its own square");
ok(!shouldFrameNumber(null), "no meta does not frame");
ok(!shouldFrameNumber(meta({ type: "int" })),
   "an int with no declared range does not frame -- nothing bounds the digits");
ok(shouldFrameNumber(meta({ type: "int", min: 0, max: FRAME_MAX_SPAN })),
   "exactly at the unipolar span limit still frames");
ok(!shouldFrameNumber(meta({ type: "int", min: 0, max: FRAME_MAX_SPAN + 1 })),
   "one past the unipolar span limit does not");
ok(shouldFrameNumber(meta({ type: "int", min: -FRAME_BIPOLAR_MAX_SPAN / 2,
                            max: FRAME_BIPOLAR_MAX_SPAN / 2 })),
   "a bipolar range gets a wider bound -- the SIGN is information an arc "
   + "cannot show, so it earns the room");
ok(!shouldFrameNumber(meta({ type: "int", min: -FRAME_BIPOLAR_MAX_SPAN,
                             max: FRAME_BIPOLAR_MAX_SPAN })),
   "but not unlimited room");

/* THE AMOUNTS. These are typed int and are NOT numbers you read -- they are
   sweeps, and an arc is the right picture. Framing them was the first version
   of this rule, and it framed 1392 params across 60 modules. */
ok(!shouldFrameNumber(meta({ type: "int", min: 0, max: 100 })),
   "a 0..100 amount (obxd volume) does not frame");
ok(!shouldFrameNumber(meta({ type: "int", min: 0, max: 127 })),
   "a 0..127 amount (9w9 tune, minijv macro_cutoff) does not frame");

/* THE TARGETS, spelled as the fleet actually declares them. */
ok(shouldFrameNumber(meta({ type: "int", min: -4, max: 4 })),
   "sfz/sf2 octave_transpose frames");
ok(shouldFrameNumber(meta({ type: "int", min: -24, max: 24 })),
   "hush1 transpose frames");
ok(shouldFrameNumber(meta({ type: "int", min: 1, max: 8 })),
   "a voice count frames");
ok(shouldFrameNumber(meta({ type: "int", min: 1, max: 16 })),
   "a MIDI channel frames");

/* --------------------------------------------------------------- the string */
ok(framedNumberText(meta({ type: "int", min: -24, max: 24 }), "2") === "+2",
   "a positive value on a bipolar range shows its +");
ok(framedNumberText(meta({ type: "int", min: -24, max: 24 }), "-1") === "-1",
   "a negative value shows its -");
ok(framedNumberText(meta({ type: "int", min: -24, max: 24 }), "0") === "0",
   "zero carries no sign");
ok(framedNumberText(meta({ type: "int", min: 1, max: 8 }), "4") === "4",
   "a unipolar range carries no + -- there is nothing to contrast with");
ok(framedNumberText(meta({ type: "int", min: -24, max: 24 }), null) === "--",
   "an unread value is --, not 0");
ok(framedNumberText(meta({ type: "int", min: -24, max: 24 }), "") === "--",
   "an unserved value is --, not 0 -- see the tri-state read contract");
ok(framedNumberText(meta({ type: "int", min: -24, max: 24 }), "abc") === "--",
   "a non-numeric answer is --, not 0");

process.exit(fail ? 1 : 0);
'

node --input-type=module -e '
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { drawFramedNumber, FRAME_W, FRAME_H }
  from "./src/shared/param_pages/render_page_movy.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };
const at = (fb, x, y) => fb.pixels[y * fb.width + x];

const X = 20, Y = 20;
const fb = createFramebuffer();
drawFramedNumber(drawContext(fb), X, Y, "+2");

/* A rectangle: all four corners lit, and the two edges continuous. */
ok(at(fb, X, Y) === 1 && at(fb, X + FRAME_W - 1, Y) === 1
   && at(fb, X, Y + FRAME_H - 1) === 1 && at(fb, X + FRAME_W - 1, Y + FRAME_H - 1) === 1,
   "all four corners of the frame are lit");
let top = 0, bottom = 0;
for (let x = X; x < X + FRAME_W; x++) {
  if (at(fb, x, Y) === 1) top++;
  if (at(fb, x, Y + FRAME_H - 1) === 1) bottom++;
}
ok(top === FRAME_W && bottom === FRAME_W, "top and bottom edges are continuous");

/* Digits inside, not on the frame. */
let inside = 0;
for (let y = Y + 1; y < Y + FRAME_H - 1; y++)
  for (let x = X + 1; x < X + FRAME_W - 1; x++) if (at(fb, x, y) === 1) inside++;
ok(inside > 4, "digits are drawn inside the frame (got " + inside + " lit)");

/* The sign must actually be DRAWN, not merely computed. A wider string must
   light more pixels than a bare digit. */
const bare = createFramebuffer();
drawFramedNumber(drawContext(bare), X, Y, "2");
ok(fb.countLit() > bare.countLit(),
   "the + is drawn, not just returned by framedNumberText");

ok(fb.clipped() === 0, "nothing was drawn off-screen");

process.exit(fail ? 1 : 0);
'

node --input-type=module -e '
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import * as RM from "./src/shared/param_pages/render_page_movy.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };
const at = (fb, x, y) => fb.pixels[y * fb.width + x];

/*
 * THE WIRING, through the real renderer.
 *
 * The assertions above exercise shouldFrameNumber, framedNumberText and
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

/* The frame is a rectangle in the first cell: find a row with a long run. */
const hasRectTop = (fb) => {
  for (let y = RM.ROW0_Y; y < RM.ROW0_Y + RM.BOX_H; y++) {
    let run = 0, best = 0;
    for (let x = 0; x < RM.CELL_W; x++) {
      run = at(fb, x, y) === 1 ? run + 1 : 0;
      if (run > best) best = run;
    }
    if (best >= RM.FRAME_W) return true;
  }
  return false;
};
ok(hasRectTop(framed),
   "a small bipolar int renders as a FRAMED cell through renderPageMovy");
ok(!hasRectTop(arced),
   "a 0..127 amount does not -- it keeps its arc");

process.exit(fail ? 1 : 0);
'
