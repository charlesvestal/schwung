#!/usr/bin/env bash
# A PAGE THE E16 SKIPS MUST NOT SHIFT WHICH PAGE A KNOB DRIVES. The E16 shows
# only pages with a knob (pageHasKnobs), but a turn moves the controller to a
# cell page BY INDEX. On Hank the controller list opens with a knobless page:
# the screen showed Main on top, the top knobs drove the knobless page and the
# BOTTOM knobs drove Main (hardware, 2026-09-24). No apostrophes in this file.
set -euo pipefail
cd "$(dirname "$0")/../.."
node --input-type=module -e '
import { createSurface } from "./src/shared/e16_surface.mjs";
import { PAGE_KNOBS } from "./src/shared/param_pages/page_plan.mjs";
let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + " got " + a + " want " + b); fails++; } else console.log("ok   " + n); };
const page = (name, keys) => ({ kind: PAGE_KNOBS, name, level: name, keys });
const turns = [];
const ctl = {
  pages: [{ kind: "preset", name: "Presets", level: "p" }, page("Main", ["a", "b"]), page("Env", ["c", "d"])],
  pageIndex: 0,
  state: { values: { a: "0.5", b: "0.5", c: "0.5", d: "0.5" } },
  meta: {},
  load() {}, tick() {},
  goToPage(i) { this.pageIndex = i; },
  onKnobTurn(slot, dir) { turns.push([this.pages[this.pageIndex].name, slot, dir]); },
};
let t = 0;
/* One tick per detent: this test is about WHICH page a knob drives, not the
 * E16 feel (E16_PULSES_PER_DETENT), which would make each tick four turns. */
const s = createSurface({ now: () => t, send: () => true, pulsesPerDetentOf: () => 1,
  chainOf: () => ({ slots: [{ synth: "hank" }, {}, {}, {}] }),
  makeController: () => ctl });
s.setEnabled(true);
for (let i = 0; i < 3; i++) { t += 25; s.tick(); }
s.feedMidi([0xF0, 0x00, 0x21, 0x5B, 0x02, 0x01, 0x53, 0xF7]);   /* the device answers */
for (let i = 0; i < 5; i++) { t += 25; s.tick(); }
s.feedMidi([0xB0, 0x01, 0x01]);            /* encoder 1 (top row, first knob) */
s.feedMidi([0xB0, 0x09, 0x01]);            /* encoder 9 (bottom row, first knob) */
eq("the top row drives the page shown on top (Main), the bottom row the next knob page",
   turns, [["Main", 0, 1], ["Env", 0, 1]]);
console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
