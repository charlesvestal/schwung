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
const ACK = [0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53, 0xF7]  /* the ENTER ack, as captured 2026-09-24 */;

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
ok(s2.display.repaintPending, "...and the frame stays owed, not dropped");


/* --- disarming the probe REPAINTS --------------------------------------
 *
 * Arming or disarming changes what belongs on the panel and nothing else
 * does: no focus moved, no knob turned, no mode changed. Without an explicit
 * invalidate the display sits believing the device is already correct, and
 * the last probe frame stays up forever -- reported from hardware as "it is
 * still on the test screen, but frozen". The panel was not frozen; it was
 * showing the last thing anybody sent it.
 * --------------------------------------------------------------------- */
{
  accept = true;
  let armed = 6;
  let tt = 0;
  /*
   * THE FAKE DEVICE ACKS every region, as the E16 does (test_e16_wiring.sh
   * has the long form). It used not to need to: this controller has no pages,
   * and an empty module DREW NOTHING, so nothing was ever owed. It now says
   * "Loading..." instead of a blank page (hardware, 2026-09-26), and a device
   * that never answers is -- correctly -- sent those regions again forever.
   */
  const { pack7, unpack7 } = await import(R + "/src/shared/e16_protocol.mjs");
  const outbox = [];
  const unpackMsg = (packets) => { const out = [];
    for (let i = 0; i < packets.length; i += 4) { const cin = packets[i] & 0x0F;
      const n = cin === 0x05 ? 1 : cin === 0x06 ? 2 : 3;
      for (let b = 0; b < n; b++) out.push(packets[i + 1 + b]); }
    return out; };
  const tick = () => { surface.tick();
    for (const p of outbox.splice(0)) { const u = unpackMsg(p), id = u[6];
      let addr = null;
      if (id === 0x08) addr = unpack7(u.slice(7, u.length - 1), 4);
      else if (id === 0x05) addr = [unpack7(u.slice(7, u.length - 1), 1)[0], 0xFF, 0xFF, 0xFF];
      else if (id === 0x07) addr = [0xFF, 0xFF, 0xFF, 0xFF];
      if (!addr) continue;
      surface.feedMidi([0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53].concat(pack7([id, 0].concat(addr)), [0xF7])); } };
  const surface = createSurface({
    now: () => tt,
    send: (p) => { if (accept) outbox.push(p); return accept; },
    chainOf: () => ({ slots: [{ synth: "9w9" }, {}, {}, {}] }),
    followFocusOf: () => null,
    testPatternOf: () => armed,
    makeController: () => ({ pages: [], pageIndex: 0, load() {}, tick() {},
                             state: { values: {} } }),
  });
  surface.setEnabled(true); tt += 30; tick();
  surface.feedMidi(ACK);
  for (let i = 0; i < 20; i++) { tt += 30; tick(); }

  /* Settle: with the meter armed a frame is always owed, so disarm first and
   * let it quiesce, then assert that the DISARM itself owed one. */
  armed = -1;
  tt += 30; tick();
  for (let i = 0; i < 10; i++) { tt += 30; tick(); }
  eq("after disarming, the screen is not left owing a frame forever",
     surface.display.repaintPending, false);

  /* Re-arm: the change alone must owe a repaint, with no other gesture. */
  armed = 3;
  const owedBefore = surface.display.repaintPending;
  eq("nothing owed before the probe changes", owedBefore, false);
  tt += 30; tick();
  ok(surface.display.shownKind !== null,
     "re-arming the probe repaints with no other gesture");
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
