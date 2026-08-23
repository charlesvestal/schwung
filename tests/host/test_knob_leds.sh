#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# KNOB LEDS: WHICH ROW AM I ON, AND WHERE IS THIS PARAMETER SET.
#
# The movy grid draws 8 parameters as two rows of four, but the hardware is one
# row of eight encoders. Nothing on the device says which physical knob drives
# which drawn cell, so the LEDs say it: knobs 1-4 white, knobs 5-8 amber. Value
# rides on top as intensity, so the row stays identifiable at every value --
# which is why the dimmest bucket is a DARK COLOUR and not zero. Zero means "no
# parameter bound here", and that distinction is the whole of "only controls
# that do something are lit".
#
# CC 71-78, AND NOTHING ELSE.
#
# The same CC carries encoder rotation IN and the indicator ring colour OUT --
# schwung-spi schwung_move_ui.h:193 ("Knob indicator ring LEDs (RGB)", "Same CC
# as encoder rotation"), and the extending-move wiki lists Knob Indicators 71-78
# under CCs in its LED table. Notes 0-7 are TOUCH SENSORS, input only.
# schwung-movy writes both because it was unsure; writing the notes half is
# eight wasted packets per change into a buffer that holds about 64.
#
# THE DIFF CACHE IS OURS, NOT input_filter`s.
#
# setLED/setButtonLED keep a module-level cache we cannot invalidate, and the
# overtake LED-clear writes straight through move_midi_internal_send without
# updating it -- so after a clear that cache claims colours the hardware no
# longer shows. We force=true past it and diff here, which makes THIS cache the
# only thing between a knob grid and 8 MIDI sends every tick.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the knob LED tests" >&2
  exit 1
fi

node --input-type=module -e '
import { knobLedColor, updateKnobLEDs, resetKnobLedCache, clearKnobLEDs,
         WHITE_LEVELS, AMBER_LEVELS, NUM_KNOB_LEDS }
  from "./src/shared/param_pages/knob_leds.mjs";
import { normalizedOf } from "./src/shared/param_pages/render_page_movy.mjs";
import * as C from "./src/shared/constants.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* ===================================================================== 1 ==
 * ROW IDENTITY. The two scales must never collide, or the thing the colour is
 * FOR stops working.
 */
ok(WHITE_LEVELS.every((c) => !AMBER_LEVELS.includes(c)),
   "no colour appears in both rows -- the rows are always distinguishable");
ok(!WHITE_LEVELS.includes(0) && !AMBER_LEVELS.includes(0),
   "no value maps to 0: an unlit knob means UNBOUND, never just quiet");

/* Every index is a real palette entry, not a number picked by eye. */
const NAMED = new Set(Object.keys(C).filter((k) => typeof C[k] === "number").map((k) => C[k]));
for (const c of WHITE_LEVELS.concat(AMBER_LEVELS))
  ok(NAMED.has(c), "colour " + c + " is a named entry in constants.mjs");

/* ===================================================================== 2 ==
 * BUCKETS.
 */
ok(knobLedColor(0, 0.0) === WHITE_LEVELS[0], "knob 1 at 0.00 is the dimmest white");
ok(knobLedColor(0, 0.5) === WHITE_LEVELS[1], "knob 1 at 0.50 is mid white");
ok(knobLedColor(3, 1.0) === WHITE_LEVELS[2], "knob 4 at 1.00 is full white");
ok(knobLedColor(4, 0.0) === AMBER_LEVELS[0], "knob 5 at 0.00 is the dimmest amber");
ok(knobLedColor(4, 0.3) === AMBER_LEVELS[1], "knob 5 at 0.30 is amber 2");
ok(knobLedColor(7, 0.6) === AMBER_LEVELS[2], "knob 8 at 0.60 is amber 3");
ok(knobLedColor(7, 1.0) === AMBER_LEVELS[3], "knob 8 at 1.00 is full amber");
ok(knobLedColor(0, null) === 0, "an unbound knob is 0");
ok(knobLedColor(4, null) === 0, "an unbound knob is 0 on the amber row too");
ok(knobLedColor(0, NaN) === 0, "a non-finite value is unbound, not 0.0 -- a knob "
   + "we could not read must not claim to be at the bottom of its range");

/* Monotonic: a rising value never gets dimmer. */
{
  let bad = 0;
  for (const row of [0, 4]) {
    let prev = -1, prevSeen = -1;
    for (let i = 0; i <= 20; i++) {
      const c = knobLedColor(row, i / 20);
      const rank = (row === 0 ? WHITE_LEVELS : AMBER_LEVELS).indexOf(c);
      if (rank < prevSeen) bad++;
      prevSeen = rank; prev = c;
    }
  }
  ok(bad === 0, "intensity never goes DOWN as the value rises");
}

/* ===================================================================== 3 ==
 * ONE ADDRESS PER KNOB, and it is the CC.
 */
{
  const sent = [];
  const io = { setButtonLED: (cc, c, force) => sent.push({ cc, c, force }),
               setLED: () => sent.push({ note: true }) };
  const vals = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8];

  resetKnobLedCache();
  updateKnobLEDs(vals, io);
  ok(sent.length === 8, "first pass writes ONE channel per knob (got " + sent.length + ")");
  ok(sent.every((w) => w.cc >= C.MoveKnob1 && w.cc <= C.MoveKnob8),
     "every write lands on CC 71-78");
  ok(sent.every((w) => !w.note),
     "nothing is written to notes 0-7 -- those are touch sensors, input only");
  ok(sent.every((w) => w.force === true),
     "force=true, to bypass input_filter`s uninvalidatable cache");

  sent.length = 0;
  updateKnobLEDs(vals, io);
  ok(sent.length === 0, "an unchanged pass writes NOTHING (got " + sent.length + ")");

  sent.length = 0;
  updateKnobLEDs([0.9, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8], io);
  ok(sent.length === 1 && sent[0].cc === C.MoveKnob1,
     "one changed knob writes exactly its own CC (got " + sent.length + ")");

  sent.length = 0;
  resetKnobLedCache();
  updateKnobLEDs([0.9, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8], io);
  ok(sent.length === 8, "resetKnobLedCache re-emits everything (got " + sent.length + ")");

  sent.length = 0;
  clearKnobLEDs(io);
  ok(sent.length === 8 && sent.every((w) => w.c === 0),
     "clearKnobLEDs darkens all 8 (got " + sent.length + ")");
}

/* ===================================================================== 4 ==
 * THE LED AND THE ARC READ THE SAME VALUE.
 *
 * The normalisation used to be an inline expression inside drawKnobWidget. A
 * second copy here would let a knob whose arc is at three-quarters light as if
 * it were at a third, and nothing on screen would say which was wrong.
 */
ok(typeof normalizedOf === "function",
   "render_page_movy exports the normaliser, so the LED cannot disagree with the arc");
ok(normalizedOf({ type: "float", min: 0, max: 1 }, "0.25") === 0.25, "float maps linearly");
ok(normalizedOf({ type: "int", min: -24, max: 24 }, "0") === 0.5, "a bipolar int centres at 0.5");
ok(normalizedOf({ type: "float", min: 0, max: 1 }, "9") === 1, "out of range clamps high");
ok(normalizedOf({ type: "float", min: 0, max: 1 }, "-9") === 0, "out of range clamps low");
ok(normalizedOf({ type: "float", min: 0, max: 1 }, "") === null,
   "an unread value is null, NOT 0 -- see the tri-state read contract");
{
  const em = { type: "enum", kind: "enum", options: ["A", "B", "C", "D", "E"] };
  ok(normalizedOf(em, "0") === 0, "enum option 0 is the bottom");
  ok(normalizedOf(em, "4") === 1, "the last enum option is the top");
  ok(normalizedOf(em, "2") === 0.5, "a middle enum option is halfway");
}

ok(NUM_KNOB_LEDS === 8, "there are 8 knob LEDs");

process.exit(fail ? 1 : 0);
'
