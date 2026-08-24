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

import { readFileSync } from "node:fs";

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
/* DERIVED FROM THE RAMP, never from its length at the time of writing. These
   assertions used to name WHITE_LEVELS[1] as "mid" and [2] as "full", which
   pinned a 3-entry ramp -- so lengthening it to 5 failed a test that was
   describing the ramp rather than the BEHAVIOUR. The behaviour is: bottom of
   range is the first entry, top is the last, and it never skips or repeats. */
const top = (r) => r[r.length - 1];
ok(knobLedColor(0, 0.0) === WHITE_LEVELS[0], "knob 1 at 0.00 is the dimmest white");
ok(knobLedColor(3, 1.0) === top(WHITE_LEVELS), "knob 4 at 1.00 is full white");
ok(knobLedColor(4, 0.0) === AMBER_LEVELS[0], "knob 5 at 0.00 is the dimmest amber");
ok(knobLedColor(7, 1.0) === top(AMBER_LEVELS), "knob 8 at 1.00 is full amber");

/* EVERY ENTRY IS REACHABLE AND THE ORDER IS MONOTONIC IN THE VALUE. The bug
   this replaces was a ramp whose third entry was darker than its second, so
   "monotonic" is asserted on the SEQUENCE the sweep produces, not on the
   palette numbers (a palette index carries no ordering). */
for (const [row, ramp, name] of [[0, WHITE_LEVELS, "white"], [4, AMBER_LEVELS, "amber"]]) {
  const seen = [];
  for (let i = 0; i <= 100; i++) {
    const c = knobLedColor(row, i / 100);
    if (seen[seen.length - 1] !== c) seen.push(c);
  }
  ok(seen.length === ramp.length,
     "sweeping the " + name + " row visits every level exactly once, got "
     + seen.length + " of " + ramp.length);
  ok(seen.join(",") === ramp.join(","),
     "and visits them IN RAMP ORDER (got " + seen.join(",") + ")");
}
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
  /* A plugin may report an enum by NAME. Number("C") is NaN, so a normaliser
     built on Number() leaves every such knob dark -- and nothing on screen
     says so, because the cell reads the name correctly either way. */
  ok(normalizedOf(em, "C") === 0.5, "an enum reported by NAME resolves to its index");
  ok(normalizedOf(em, "E") === 1, "the last option by name is the top");
  ok(normalizedOf(em, "nosuch") === null, "an unrecognised option name is unknown, not 0");
  ok(knobLedColor(0, normalizedOf(em, "E")) === WHITE_LEVELS[WHITE_LEVELS.length - 1],
     "a name-reporting enum at its last option lights FULL, not dark");
}

/* ===================================================================== N ==
 * THE RAMPS ARE MONOTONIC IN ACTUAL BRIGHTNESS.
 *
 * Everything above checks that a sweep walks the ramp in the order the ramp is
 * WRITTEN. That was true of the broken version too: DarkBrown2 -> Mustard ->
 * Ochre -> BrightOrange is #250E05 -> #876700 -> #491804 -> #C93C00, and the
 * third entry is darker than the second. A knob swept min to max went dim,
 * bright, dark, bright, and every test passed.
 *
 * So read the HEX out of the palette header in constants.mjs -- the same table
 * a person would read to pick a colour -- and require luminance to increase.
 * This is the assertion that would have caught it.
 */
{
  const src = readFileSync("./src/shared/constants.mjs", "utf8");

  /* Lines look like:
   *     3 : #C93C00  Bright Orange   dim  69 #5D1700   dark  70 #200D00
   *   118 : #595959  Light Grey
   * so every "<index> #RRGGBB" pair on a line is an entry, including the dim
   * and dark variants, which is exactly where a ramp should be drawing from. */
  const hex = new Map();
  for (const m of src.matchAll(/(\d{1,3})\s*:?\s*#([0-9A-Fa-f]{6})/g))
    if (!hex.has(Number(m[1]))) hex.set(Number(m[1]), m[2]);
  for (const m of src.matchAll(/(?:dim|dark)\s+(\d{1,3})\s+#([0-9A-Fa-f]{6})/g))
    if (!hex.has(Number(m[1]))) hex.set(Number(m[1]), m[2]);

  ok(hex.size > 100, "the palette header was parsed, got " + hex.size + " entries");

  /* Rec. 709 relative luminance. Any sane weighting orders these the same; the
     point is that #876700 must not sort below #491804. */
  const lum = (h) => {
    const r = parseInt(h.slice(0, 2), 16), g = parseInt(h.slice(2, 4), 16),
          b = parseInt(h.slice(4, 6), 16);
    return 0.2126 * r + 0.7152 * g + 0.0722 * b;
  };

  for (const [ramp, name] of [[WHITE_LEVELS, "white"], [AMBER_LEVELS, "amber"]]) {
    const missing = ramp.filter((c) => !hex.has(c));
    ok(missing.length === 0,
       "every " + name + " level appears in the palette header, missing ["
       + missing.join(",") + "]");
    if (missing.length) continue;
    const ls = ramp.map((c) => lum(hex.get(c)));
    let rising = true;
    for (let i = 1; i < ls.length; i++) if (ls[i] <= ls[i - 1]) rising = false;
    ok(rising, "the " + name + " ramp gets STRICTLY brighter every step: "
       + ramp.map((c, i) => "#" + hex.get(c) + "(" + ls[i].toFixed(0) + ")").join(" -> "));
  }

  /* THE ROWS MUST STAY TELLABLE APART, which is the whole reason there are two
     ramps. Brightness alone cannot do it -- a dim white and a bright amber can
     land at the same luminance -- so require a hue difference at every level. */
  const warmth = (h) => parseInt(h.slice(0, 2), 16) - parseInt(h.slice(4, 6), 16);
  const whiteWarm = WHITE_LEVELS.map((c) => warmth(hex.get(c)));
  const amberWarm = AMBER_LEVELS.map((c) => warmth(hex.get(c)));
  ok(Math.max.apply(null, whiteWarm) < Math.min.apply(null, amberWarm),
     "every amber level is warmer than every white level, so the two rows are "
     + "told apart by HUE at any brightness");
}

ok(NUM_KNOB_LEDS === 8, "there are 8 knob LEDs");

process.exit(fail ? 1 : 0);
'
