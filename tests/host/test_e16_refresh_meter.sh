#!/usr/bin/env bash
# The refresh meter (test pattern 6) -- the instrument that separates a slow
# SCREEN from slow VALUES.
#
# Those are different subsystems with different costs and no shared knob: a
# repaint is 394 paced packets on the wire, while a value is an IPC read at
# ~2.8 ms served on the controller's rotation of about one key per tick. Watching
# parameter numbers move measures their SUM, which is why "the refresh is so
# slow" could not be told apart from "the values lag" by looking -- and why
# changing SLOTS, a repaint with no value rotation in front of it, already felt
# quick.
#
# Two properties, and the meter is worthless without either:
#
#   1. IT COUNTS COMPLETED SENDS, never intents. A refused send is not a paint;
#      counting invalidations would report a rate the device never achieved,
#      which is the single answer an instrument like this must not give.
#   2. IT TOUCHES NO PARAMETER. If any element came from a value, the meter
#      would measure the sum it exists to decompose.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
const R = process.cwd();
const { createSurface } = await import(R + "/src/shared/e16_surface.mjs");
const { drawTestPattern } = await import(R + "/src/shared/e16_view.mjs");
const { createCanvas } = await import(R + "/src/shared/e16_canvas.mjs");

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + " got " + a + " want " + b); fails++; }
  else console.log("ok   " + n); };
const ok = (c, m) => { if (!c) { console.log("FAIL " + m); fails++; } else console.log("ok   " + m); };

/* --- it draws, and the reading is legible ---------------------------- */
const ink = (meter) => { const cv = createCanvas();
  drawTestPattern(cv, 6, meter); let n = 0;
  for (const b of cv.toBuffer()) if (b) n++; return n; };
ok(ink({ paints: 37, fps: 12.4 }) > 0, "the meter draws something");
ok(ink({ paints: 37, fps: 12.4 }) !== ink({ paints: 38, fps: 12.4 }),
   "and it CHANGES per paint -- a still meter cannot show a stalled wire");
ok(ink({}) > 0, "it draws before any rate is known, rather than blanking");

/* --- it counts COMPLETED sends, not intents --------------------------- */
let t = 0;
let accept = true;
const mk = () => createSurface({
  now: () => t,
  send: () => accept,
  chainOf: () => ({ slots: [{ synth: "9w9" }, {}, {}, {}] }),
  followFocusOf: () => null,
  testPatternOf: () => 6,
  makeController: () => ({ pages: [], pageIndex: 0, load() {}, tick() {},
                           state: { values: {} } }),
});
const ACK = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x06, 0x53, 0xF7];

const s1 = mk();
s1.setEnabled(true); t += 30; s1.tick(); s1.feedMidi(ACK);
for (let i = 0; i < 50; i++) { t += 30; s1.tick(); }
const withSends = s1.display.shownKind;
ok(withSends !== null, "an accepted send leaves the device in a known mode");

/* Now refuse every send: the meter must NOT advance. A rate that climbs while
 * the wire refuses is the precise lie this test exists to prevent. */
accept = false;
const s2 = mk();
s2.setEnabled(true); t += 30; s2.tick(); s2.feedMidi(ACK);
for (let i = 0; i < 50; i++) { t += 30; s2.tick(); }
eq("a refused send is not a paint", s2.display.shownKind, null);
ok(s2.display.framebufferOwed, "...and the frame stays owed, not dropped");

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
